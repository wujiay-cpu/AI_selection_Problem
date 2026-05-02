import bisect
from collections import OrderedDict
import heapq
import importlib
import itertools
import math
import os
import threading
import time

# ============================================================
#  尝试加载 C++ 加速模块
#  成功：required_cover==1 的贪心路径走 C++ 实现
#  失败：自动回退到纯 Python 实现，打印一次警告
# ============================================================
_cover_core = None
_CPP_AVAILABLE = False
_CPP_MODULE_NAME = None

# 只接受真正具备加速接口的模块，避免把 backend.cover_core 包误判为扩展模块
_REQUIRED_ACCEL_ATTRS = ("greedy_selection", "backtracking_selection", "beam_search_selection")


def _is_valid_accel_module(mod):
    return all(hasattr(mod, attr) for attr in _REQUIRED_ACCEL_ATTRS)


# 优先 cover_core_ext（pybind11 直接产物），并兼容 backend/cover_core 目录下的编译产物
for _mod_name in ("backend.cover_core_ext", "backend.cover_core.cover_core_ext", "cover_core_ext", "cover_core"):
    try:
        _candidate = importlib.import_module(_mod_name)
        if _is_valid_accel_module(_candidate):
            _cover_core = _candidate
            _CPP_AVAILABLE = True
            _CPP_MODULE_NAME = _mod_name
            break
    except ImportError:
        continue

if _CPP_AVAILABLE:
    print(f"[cover_core] C++ 加速模块加载成功 (module={_CPP_MODULE_NAME})")
else:
    print("[cover_core] 未找到 C++ 模块，使用纯 Python 实现")


def _read_env_flag(name, default=True):
    raw = os.getenv(name)
    if raw is None:
        return default
    val = str(raw).strip().lower()
    if val in {"1", "true", "yes", "on"}:
        return True
    if val in {"0", "false", "no", "off"}:
        return False
    return default


# 可通过环境变量覆盖，默认开启
# 示例：COVER_CPP_ULTRA_ENABLED=0 uvicorn backend.api:app ...
_CPP_ULTRA_ENABLED = _read_env_flag("COVER_CPP_ULTRA_ENABLED", True)

# 预计算缓存：复用 targets/index/candidates，减少重复构建开销
_PRECOMP_CACHE_ENABLED = _read_env_flag("COVER_PRECOMP_CACHE_ENABLED", True)
_PRECOMP_CACHE_SIZE = max(2, int(os.getenv("COVER_PRECOMP_CACHE_SIZE", "24")))
_PRECOMP_CACHE_LOCK = threading.RLock()
_TARGET_INDEX_CACHE = OrderedDict()      # key: (pool_tuple, j, s)
_BITMASK_CANDS_CACHE = OrderedDict()     # key: (pool_tuple, k, j, s)
_MULTI_CANDS_CACHE = OrderedDict()       # key: (pool_tuple, k, j, s)


def _cache_get(cache_obj, key):
    if not _PRECOMP_CACHE_ENABLED:
        return None
    with _PRECOMP_CACHE_LOCK:
        val = cache_obj.get(key)
        if val is not None:
            cache_obj.move_to_end(key)
        return val


def _cache_set(cache_obj, key, value):
    if not _PRECOMP_CACHE_ENABLED:
        return value
    with _PRECOMP_CACHE_LOCK:
        cache_obj[key] = value
        cache_obj.move_to_end(key)
        while len(cache_obj) > _PRECOMP_CACHE_SIZE:
            cache_obj.popitem(last=False)
    return value


def _get_targets_and_subset_inverted_index_cached(sample_pool, j, s):
    key = (tuple(sample_pool), int(j), int(s))
    cached = _cache_get(_TARGET_INDEX_CACHE, key)
    if cached is not None:
        return cached
    return _cache_set(_TARGET_INDEX_CACHE, key, build_targets_and_subset_inverted_index(sample_pool, j, s))


def _get_unique_candidates_bitmask_cached(sample_pool, k, j, s, sub_to_target_mask):
    key = (tuple(sample_pool), int(k), int(j), int(s))
    cached = _cache_get(_BITMASK_CANDS_CACHE, key)
    if cached is not None:
        return cached
    built = build_unique_candidates_bitmask(sample_pool, k, s, sub_to_target_mask)
    return _cache_set(_BITMASK_CANDS_CACHE, key, built)


def _get_candidates_cover_maps_cached(sample_pool, k, j, s, all_targets):
    key = (tuple(sample_pool), int(k), int(j), int(s))
    cached = _cache_get(_MULTI_CANDS_CACHE, key)
    if cached is None:
        cached = _cache_set(
            _MULTI_CANDS_CACHE,
            key,
            build_candidates_cover_maps(sample_pool, k, all_targets, s),
        )
    # 调用方会 pop/sort，返回一份浅拷贝防止污染缓存本体
    return list(cached)


def resolve_required_cover(j, s, min_cover):
    total = math.comb(j, s)

    if s == j:
        return 1

    if min_cover is None or min_cover == "":
        return 1

    if isinstance(min_cover, str):
        if min_cover.lower() == "all":
            return total
        min_cover = int(min_cover)

    required = int(min_cover)
    if required < 1:
        required = 1
    if required > total:
        required = total
    return required

def build_targets_and_subset_inverted_index(sample_pool, j, s):
    all_targets = list(itertools.combinations(sample_pool, j))
    sub_to_target_mask = {}
    for ti, t in enumerate(all_targets):
        bit = 1 << ti
        for sub in itertools.combinations(t, s):
            sub_to_target_mask[sub] = sub_to_target_mask.get(sub, 0) | bit
    return all_targets, sub_to_target_mask


def build_unique_candidates_bitmask(sample_pool, k, s, sub_to_target_mask):
    mask_to_candidate = {}
    for c in itertools.combinations(sample_pool, k):
        cover_mask = 0
        for sub in itertools.combinations(c, s):
            cover_mask |= sub_to_target_mask.get(sub, 0)
        if cover_mask and cover_mask not in mask_to_candidate:
            mask_to_candidate[cover_mask] = c
    return [(c, cover_mask, cover_mask.bit_count()) for cover_mask, c in mask_to_candidate.items()]


def build_candidates_cover_maps(sample_pool, k, all_targets, s):
    candidate_cover_maps = []
    for c in itertools.combinations(sample_pool, k):
        c_set = set(c)
        cover_map = {}
        for idx, t in enumerate(all_targets):
            inter = c_set.intersection(t)
            if len(inter) >= s:
                cover_map[idx] = set(itertools.combinations(sorted(inter), s))
        if cover_map:
            candidate_cover_maps.append((c, cover_map, sum(len(v) for v in cover_map.values())))
    return candidate_cover_maps


def get_heuristic_greedy_selection(sample_pool, k, j, s, min_cover=1):
    required_cover = resolve_required_cover(j, s, min_cover)

    # ============================================================
    #  required_cover == 1：优先走 C++ 加速路径
    # ============================================================
    if required_cover == 1:
        if _CPP_AVAILABLE and _CPP_ULTRA_ENABLED:
            try:
                if hasattr(_cover_core, "greedy_fast_selection"):
                    res, _ = _cover_core.greedy_fast_selection(list(sample_pool), k, j, s)
                else:
                    res, _ = _cover_core.greedy_selection(list(sample_pool), k, j, s)
                if res:
                    return res
            except Exception as e:
                print(f"C++ Greedy Error: {e}")
        # --- 纯 Python 回退 ---
        return _greedy_selection_python(sample_pool, k, j, s)

    # ============================================================
    #  required_cover > 1：纯 Python（此分支不在本次 C++ 迁移范围内）
    # ============================================================
    return _greedy_selection_multi_cover(sample_pool, k, j, s, required_cover)


# ============================================================
#  纯 Python 实现（required_cover == 1）
#  与改动前完全一致，仅在 cover_core 不可用时作为回退
# ============================================================
def _greedy_selection_python(sample_pool, k, j, s):
    all_targets, sub_to_target_mask = _get_targets_and_subset_inverted_index_cached(sample_pool, j, s)
    target_count = len(all_targets)
    candidates = _get_unique_candidates_bitmask_cached(sample_pool, k, j, s, sub_to_target_mask)

    n = len(sample_pool)
    all_mask = (1 << target_count) - 1

    # 目标稀缺度权重
    target_freq = [0] * target_count
    for _, cmask, _ in candidates:
        bits = cmask
        while bits:
            bit = bits & -bits
            bits -= bit
            target_freq[bit.bit_length() - 1] += 1
    target_weight = [1.0 / max(f, 1) for f in target_freq]

    cand_weighted_score = []
    for _, cmask, _ in candidates:
        score = 0.0
        bits = cmask
        while bits:
            bit = bits & -bits
            bits -= bit
            score += target_weight[bit.bit_length() - 1]
        cand_weighted_score.append(score)

    if n >= 21 and len(candidates) > 20000:
        top_ids = {i for i, _ in heapq.nlargest(20000, enumerate(candidates), key=lambda x: x[1][2])}
        phase1 = [i for i in range(len(candidates)) if i in top_ids]
        phase2 = [i for i in range(len(candidates)) if i not in top_ids]
    else:
        phase1 = list(range(len(candidates)))
        phase2 = []

    def compute_weighted_gain(i, covered_mask):
        _, cmask, _ = candidates[i]
        add_mask = cmask & ~covered_mask
        score = 0.0
        bits = add_mask
        while bits:
            bit = bits & -bits
            bits -= bit
            score += target_weight[bit.bit_length() - 1]
        return score

    def run_phase(indexes, covered_mask, remaining, selected_ids):
        if remaining <= 0 or not indexes:
            return covered_mask, remaining
        heap = []
        for i in indexes:
            _, cmask, bc = candidates[i]
            wg = cand_weighted_score[i]
            heapq.heappush(heap, (-bc, -wg, i))
        active = {i: True for i in indexes}

        while remaining > 0 and heap:
            neg_gain, neg_wg, i = heapq.heappop(heap)
            if not active.get(i, False):
                continue
            _, cmask, _ = candidates[i]
            add_mask = cmask & ~covered_mask
            real_gain = add_mask.bit_count()
            if real_gain == 0:
                active[i] = False
                continue
            if real_gain != -neg_gain:
                new_wg = compute_weighted_gain(i, covered_mask)
                heapq.heappush(heap, (-real_gain, -new_wg, i))
                continue
            active[i] = False
            selected_ids.append(i)
            covered_mask |= add_mask
            remaining -= real_gain
        return covered_mask, remaining

    def prune_redundant(selected_ids):
        while True:
            to_remove = -1
            m = len(selected_ids)
            if m <= 1:
                break
            current_full_mask = 0
            for cid in selected_ids:
                current_full_mask |= candidates[cid][1]
            for i in range(m):
                mask_without_i = 0
                for j in range(m):
                    if i == j: continue
                    mask_without_i |= candidates[selected_ids[j]][1]
                if mask_without_i == current_full_mask:
                    to_remove = i
                    break
            if to_remove != -1:
                selected_ids.pop(to_remove)
            else:
                break
        return selected_ids

    def try_replace_2for1(selected_ids):
        changed = True
        while changed:
            changed = False
            m = len(selected_ids)
            if m < 2:
                break
            full_mask = 0
            for cid in selected_ids:
                full_mask |= candidates[cid][1]
            for ii in range(m):
                for jj in range(ii + 1, m):
                    rest_mask = 0
                    for kk in range(m):
                        if kk == ii or kk == jj:
                            continue
                        rest_mask |= candidates[selected_ids[kk]][1]
                    gap_mask = all_mask & ~rest_mask
                    if gap_mask == 0:
                        new_ids = [cid for kk, cid in enumerate(selected_ids) if kk != ii and kk != jj]
                        selected_ids = new_ids
                        changed = True
                        break
                    selected_set = set(selected_ids)
                    best_ci = -1
                    for ci in range(len(candidates)):
                        if ci in selected_set:
                            continue
                        if (candidates[ci][1] & gap_mask) == gap_mask:
                            best_ci = ci
                            break
                    if best_ci != -1:
                        new_ids = [cid for kk, cid in enumerate(selected_ids) if kk != ii and kk != jj]
                        new_ids.append(best_ci)
                        selected_ids = new_ids
                        changed = True
                        break
                if changed:
                    break
        return selected_ids

    remaining = target_count
    covered_mask = 0
    selected_ids = []
    print(f"开始计算... 共有 {target_count} 个目标需要覆盖")

    covered_mask, remaining = run_phase(phase1, covered_mask, remaining, selected_ids)
    if remaining > 0:
        covered_mask, remaining = run_phase(phase2, covered_mask, remaining, selected_ids)

    if remaining == 0:
        selected_ids = prune_redundant(selected_ids)
        before_2opt = len(selected_ids)
        selected_ids = try_replace_2for1(selected_ids)
        selected_ids = prune_redundant(selected_ids)
        after_2opt = len(selected_ids)
        if after_2opt < before_2opt:
            print(f"2-opt 替换优化：{before_2opt} -> {after_2opt} 组")
    else:
        print("警告：无法继续覆盖")

    return [sorted(list(candidates[i][0])) for i in selected_ids]


# ============================================================
#  required_cover > 1 的纯 Python 贪心（不在 C++ 迁移范围内）
# ============================================================
def _greedy_selection_multi_cover(sample_pool, k, j, s, required_cover):
    all_targets, _ = _get_targets_and_subset_inverted_index_cached(sample_pool, j, s)
    candidate_cover_maps = _get_candidates_cover_maps_cached(sample_pool, k, j, s, all_targets)

    final_result = []
    covered_subsets = [set() for _ in all_targets]
    print(f"开始计算... 共有 {len(all_targets)} 个目标需要覆盖")

    target_freq_multi = [0] * len(all_targets)
    for _, cover_map, _ in candidate_cover_maps:
        for idx in cover_map:
            target_freq_multi[idx] += 1
    target_weight_multi = [1.0 / max(f, 1) for f in target_freq_multi]

    while True:
        done = True
        for idx in range(len(all_targets)):
            if len(covered_subsets[idx]) < required_cover:
                done = False
                break
        if done:
            break

        best_idx = -1
        best_gain = 0
        best_weighted_gain = -1.0

        for i, (_, cover_map, _) in enumerate(candidate_cover_maps):
            gain = 0
            weighted_gain = 0.0
            for idx, add_set in cover_map.items():
                before = min(required_cover, len(covered_subsets[idx]))
                after = min(required_cover, len(covered_subsets[idx].union(add_set)))
                delta = after - before
                gain += delta
                weighted_gain += delta * target_weight_multi[idx]
            if gain > best_gain or (gain == best_gain and gain > 0 and weighted_gain > best_weighted_gain):
                best_gain = gain
                best_weighted_gain = weighted_gain
                best_idx = i

        if best_idx == -1 or best_gain == 0:
            print("警告：无法继续覆盖")
            break

        best_candidate, best_cover_map, _ = candidate_cover_maps.pop(best_idx)
        final_result.append(sorted(list(best_candidate)))
        for idx, add_set in best_cover_map.items():
            covered_subsets[idx].update(add_set)

    final_result = _prune_redundant_multi(final_result, sample_pool, k, all_targets, s, required_cover)
    return final_result


def _prune_redundant_multi(final_result, sample_pool, k, all_targets, s, required_cover):
    if not final_result:
        return final_result

    def build_cover_map_for(cand):
        c_set = set(cand)
        cover_map = {}
        for idx, t in enumerate(all_targets):
            inter = c_set.intersection(t)
            if len(inter) >= s:
                cover_map[idx] = set(itertools.combinations(sorted(inter), s))
        return cover_map

    cover_maps = [build_cover_map_for(tuple(c)) for c in final_result]

    changed = True
    while changed:
        changed = False
        m = len(final_result)
        if m <= 1:
            break
        for i in range(m):
            covered_without_i = [set() for _ in all_targets]
            for j in range(m):
                if j == i:
                    continue
                for idx, add_set in cover_maps[j].items():
                    covered_without_i[idx].update(add_set)
            still_ok = all(len(covered_without_i[idx]) >= required_cover for idx in range(len(all_targets)))
            if still_ok:
                final_result.pop(i)
                cover_maps.pop(i)
                changed = True
                break

    return final_result


def run_heuristic_greedy(m, n, k, j, s, min_cover, selected_numbers):
    return get_heuristic_greedy_selection(selected_numbers, k, j, s, min_cover)


def run_backtracking_pruning(m, n, k, j, s, min_cover, selected_numbers, progress_callback=None,
                             greedy_result=None, emit_greedy_progress=True, time_budget_seconds=90.0):
    if greedy_result is None:
        greedy_result = get_heuristic_greedy_selection(selected_numbers, k, j, s, min_cover)
    if progress_callback and emit_greedy_progress:
        progress_callback({"stage": "greedy", "best_size": len(greedy_result), "result": greedy_result})

    meta = improve_with_backtracking_status(
        selected_numbers, k, j, s, min_cover, greedy_result, time_budget_seconds, progress_callback=progress_callback
    )
    improved = meta.get("result", [])
    aborted = meta.get("aborted", False)

    if improved and len(improved) < len(greedy_result):
        print(f"回溯改进成功：{len(greedy_result)} -> {len(improved)} 组")
        return improved, aborted
    return greedy_result, aborted


def improve_with_backtracking_status(sample_pool, k, j, s, min_cover, greedy_result, time_budget_seconds=90.0, progress_callback=None):
    if not greedy_result:
        return {"result": [], "aborted": False, "proved_optimal": False}
    upper_bound_groups = len(greedy_result) - 1
    if upper_bound_groups <= 0:
        return {"result": [], "aborted": False, "proved_optimal": True}

    meta = get_backtracking_selection(
        sample_pool, k, j, s, min_cover,
        max_seconds=time_budget_seconds,
        upper_bound_groups=upper_bound_groups,
        fallback_to_greedy=False,
        return_meta=True,
        progress_callback=progress_callback,
    )
    
    # 如果回溯没有找到更好的解，并且被 abort 了，此时返回的 meta["result"] 可能是空的
    # 我们不希望外层因为收到空 result 而崩溃。调用方应该拿到空 list，然后保留 greedy_result。
    return {
        "result": meta.get("result", []),
        "aborted": meta.get("aborted", False),
        "proved_optimal": (not meta.get("aborted", False)) and (not meta.get("result")),
    }


def improve_with_backtracking(sample_pool, k, j, s, min_cover, greedy_result, time_budget_seconds=90.0, progress_callback=None):
    return improve_with_backtracking_status(
        sample_pool, k, j, s, min_cover, greedy_result, time_budget_seconds, progress_callback=progress_callback
    ).get("result", [])


def get_backtracking_operation_limit(n, required_cover):
    if n <= 10:
        base = 90000000
    elif n <= 12:
        base = 110000000
    elif n <= 15:
        base = 115000000
    else:
        base = 120000000

    if required_cover <= 1:
        factor = 1.0
    elif required_cover == 2:
        factor = 1.25
    else:
        factor = 1.5
    return int(base * factor)


def get_backtracking_selection_bitmask(sample_pool, k, j, s, max_ops, max_seconds,
                                       upper_bound_groups=None, fallback_to_greedy=True,
                                       return_meta=False, progress_callback=None):
    # 如果 C++ 模块可用，直接委托给 C++ 执行带剪枝的 DFS 和 3-Opt
    if _CPP_AVAILABLE and _CPP_ULTRA_ENABLED:
        try:
            print(f"Calling C++ backtracking with time_limit={max_seconds}s...")
            bt_results, aborted = _cover_core.backtracking_selection(
                list(sample_pool), k, j, s, float(max_seconds)
            )
            print(f"C++ backtracking finished. Result size: {len(bt_results)}, Aborted: {aborted}")
            
            # C++ 模块已经做过 3-Opt 兜底，直接返回结果
            if return_meta:
                return {"result": bt_results, "aborted": aborted}
            return bt_results, aborted
        except Exception as e:
            print(f"C++ backtracking failed: {e}. Falling back to Python implementation.")

    # --- 以下为原有的 Python DFS 回退逻辑 ---
    all_targets, sub_to_target_mask = _get_targets_and_subset_inverted_index_cached(sample_pool, j, s)
    target_count = len(all_targets)
    all_mask = (1 << target_count) - 1

    bitmask_candidates = _get_unique_candidates_bitmask_cached(sample_pool, k, j, s, sub_to_target_mask)

    canonical_c1 = tuple(sorted(sample_pool[:k]))
    canonical_item = None
    other_items = []
    for cand, cmask, bc in bitmask_candidates:
        if tuple(sorted(cand)) == canonical_c1 and canonical_item is None:
            canonical_item = (cand, cmask, bc)
        else:
            other_items.append((cand, cmask, bc))

    raw = sorted(other_items, key=lambda x: x[2], reverse=True)
    pruned_others = []
    for cand, cmask, bc in raw:
        dominated = False
        for _, kmask, _ in pruned_others:
            if (cmask | kmask) == kmask:
                dominated = True
                break
        if not dominated:
            pruned_others.append((cand, cmask, bc))

    if canonical_item is not None:
        candidates = [canonical_item] + pruned_others
    else:
        candidates = pruned_others

    candidate_count = len(candidates)
    bit_to_candidates = [[] for _ in range(target_count)]
    for ci, (_, cmask, _) in enumerate(candidates):
        bits = cmask
        while bits:
            bit = bits & -bits
            bits -= bit
            bit_to_candidates[bit.bit_length() - 1].append(ci)

    max_single_gain_global = max((bc for _, _, bc in candidates), default=0)
    lower_bound = (target_count + max_single_gain_global - 1) // max_single_gain_global if max_single_gain_global > 0 else 0

    answer = []
    ops = 0
    aborted = False
    start_time = time.perf_counter()
    deadline = start_time + max_seconds
    memo = {}
    stats = {
        "cut_deadline": 0, "cut_max_ops": 0, "cut_terminal": 0,
        "cut_future": 0, "cut_memo": 0, "cut_lb_single": 0, "nodes": 0,
    }

    future_masks = [0] * (candidate_count + 1)
    for i in range(candidate_count - 1, -1, -1):
        future_masks[i] = future_masks[i+1] | candidates[i][1]

    def feasible(start, covered_mask):
        return (covered_mask | future_masks[start]) == all_mask

    def dfs(start, covered_mask, left, picked):
        nonlocal answer, ops, aborted
        if aborted:
            return False
        if time.perf_counter() >= deadline:
            stats["cut_deadline"] += 1
            aborted = True
            return False
        ops += 1
        stats["nodes"] += 1
        if max_ops is not None and ops >= max_ops:
            stats["cut_max_ops"] += 1
            aborted = True
            return False
        if covered_mask == all_mask:
            answer = [list(x) for x in picked]
            return True
        if left == 0 or start >= candidate_count:
            stats["cut_terminal"] += 1
            return False
        if not feasible(start, covered_mask):
            stats["cut_future"] += 1
            return False

        key = (start, covered_mask)
        if memo.get(key, -1) >= left:
            stats["cut_memo"] += 1
            return False
        memo[key] = left

        uncovered = all_mask & ~covered_mask
        need = uncovered.bit_count()

        proj = []
        for ci in range(start, candidate_count):
            pm = candidates[ci][1] & uncovered
            if pm == 0: continue
            proj.append((pm.bit_count(), ci, pm))

        if not proj:
            return False

        proj.sort(key=lambda x: x[0], reverse=True)

        best = proj[0][0]
        if (need + best - 1) // best > left:
            stats["cut_lb_single"] += 1
            return False

        kept = []
        for g, ci, pm in proj:
            dominated = False
            for _, _, km in kept:
                if (pm | km) == km:
                    dominated = True
                    break
            if not dominated:
                kept.append((g, ci, pm))

        for _, ci, _ in kept:
            cand, add, _ = candidates[ci]
            picked.append(cand)
            if dfs(ci + 1, covered_mask | add, left - 1, picked):
                return True
            picked.pop()
        return False

    max_size = candidate_count if upper_bound_groups is None else min(candidate_count, max(0, upper_bound_groups))
    ops_label = "time-only" if max_ops is None else str(max_ops)
    best_answer = []
    print(f"开始回溯搜索 (位运算加速)... 共有 {target_count} 个目标，{candidate_count} 个有效候选组，"
          f"运算上限 {ops_label}，时间上限 {max_seconds:.1f}s，下界LB={lower_bound}")

    for size in range(max_size, 0, -1):
        if time.perf_counter() >= deadline:
            aborted = True
            break
        memo.clear()
        layer_start = time.perf_counter()
        nodes_before = stats["nodes"]
        print(f"尝试 size={size} ...")
        found_this_size = dfs(0, 0, size, [])
        layer_cost = time.perf_counter() - layer_start
        layer_nodes = stats["nodes"] - nodes_before
        if found_this_size:
            best_answer = [list(x) for x in answer]
            print(f"size={size} 找到可行解，用时 {layer_cost:.2f}s，节点 {layer_nodes}")
            if progress_callback:
                progress_callback({"stage": "backtracking", "best_size": size,
                                   "result": best_answer, "elapsed": time.perf_counter() - start_time})
            if lower_bound > 0 and size <= lower_bound:
                print(f"达到下界LB={lower_bound}，提前停止搜索")
                break
        else:
            print(f"size={size} 未找到，用时 {layer_cost:.2f}s，节点 {layer_nodes}")
            if best_answer and (not aborted):
                print(f"size={size} 层完整搜索仍未找到，提前停止向更小size继续搜索")
                break

    if best_answer:
        if return_meta:
            return {"result": best_answer, "aborted": aborted}
        return best_answer, aborted

    if aborted:
        print("回溯达到上限，停止搜索")
        print(f"调试统计: nodes={stats['nodes']}, cut_deadline={stats['cut_deadline']}, "
              f"cut_max_ops={stats['cut_max_ops']}, cut_future={stats['cut_future']}, "
              f"cut_memo={stats['cut_memo']}, cut_lb_single={stats['cut_lb_single']}")
        if fallback_to_greedy:
            result = get_heuristic_greedy_selection(sample_pool, k, j, s, 1)
            if return_meta:
                return {"result": result, "aborted": True}
            return result, True
        if return_meta:
            return {"result": [], "aborted": True}
        return [], True

    print(f"调试统计: nodes={stats['nodes']}, cut_deadline={stats['cut_deadline']}, "
          f"cut_max_ops={stats['cut_max_ops']}, cut_future={stats['cut_future']}, "
          f"cut_memo={stats['cut_memo']}, cut_lb_single={stats['cut_lb_single']}")
    if return_meta:
        return {"result": answer, "aborted": False}
    return answer, False


def get_backtracking_selection(sample_pool, k, j, s, min_cover=1, max_seconds=20.0,
                               upper_bound_groups=None, fallback_to_greedy=True,
                               return_meta=False, progress_callback=None):
    n = len(sample_pool)
    need_cover = resolve_required_cover(j, s, min_cover)
    max_ops = get_backtracking_operation_limit(n, need_cover)

    if need_cover == 1:
        return get_backtracking_selection_bitmask(
            sample_pool, k, j, s, max_ops, max_seconds,
            upper_bound_groups, fallback_to_greedy, return_meta,
            progress_callback=progress_callback)

    targets, _ = _get_targets_and_subset_inverted_index_cached(sample_pool, j, s)
    candidates = _get_candidates_cover_maps_cached(sample_pool, k, j, s, targets)
    candidates.sort(key=lambda c: sum(len(v) for v in c[1].values()), reverse=True)

    target_count = len(targets)
    candidate_count = len(candidates)
    answer = []
    ops = 0
    aborted = False
    deadline = time.perf_counter() + max_seconds

    def ok(covered):
        for ti in range(target_count):
            if len(covered[ti]) < need_cover:
                return False
        return True

    def feasible(start, covered, left):
        for ti in range(target_count):
            need = need_cover - len(covered[ti])
            if need <= 0:
                continue
            gains = []
            for ci in range(start, candidate_count):
                g = len(candidates[ci][1].get(ti, set()))
                if g > 0:
                    gains.append(g)
            if sum(sorted(gains, reverse=True)[:left]) < need:
                return False
        return True

    def dfs(start, covered, left, picked):
        nonlocal answer, ops, aborted
        if aborted:
            return False
        if time.perf_counter() >= deadline:
            aborted = True
            return False
        ops += 1
        if max_ops is not None and ops >= max_ops:
            aborted = True
            return False
        if ok(covered):
            answer = [list(x) for x in picked]
            return True
        if left == 0 or start >= candidate_count:
            return False
        if not feasible(start, covered, left):
            return False

        for ci in range(start, candidate_count):
            cand, cover, _ = candidates[ci]
            added_subsets = [[] for _ in range(target_count)]
            useful = False
            for ti, sset in cover.items():
                if len(covered[ti]) >= need_cover:
                    continue
                newly_added = sset - covered[ti]
                if newly_added:
                    useful = True
                    covered[ti].update(newly_added)
                    added_subsets[ti].extend(newly_added)
            if not useful:
                continue
            picked.append(cand)
            if dfs(ci + 1, covered, left - 1, picked):
                return True
            picked.pop()
            for ti, subsets_to_remove in enumerate(added_subsets):
                if subsets_to_remove:
                    covered[ti].difference_update(subsets_to_remove)
        return False

    max_size = candidate_count if upper_bound_groups is None else min(candidate_count, max(0, upper_bound_groups))
    ops_label = "time-only" if max_ops is None else str(max_ops)
    print(f"开始回溯搜索... 共有 {len(targets)} 个目标，{len(candidates)} 个有效候选组，"
          f"运算上限 {ops_label}，时间上限 {max_seconds:.1f}s")
    for size in range(1, max_size + 1):
        if dfs(0, [set() for _ in range(target_count)], size, []):
            print(f"找到最优解，组数: {size}")
            if return_meta:
                return {"result": answer, "aborted": False}
            return answer, False
    if return_meta:
        return {"result": answer, "aborted": aborted}
    return answer, aborted


def run_algorithm(m, n, k, j, s, min_cover, selected_numbers, optimization_level=2,
                  progress_callback=None, initial_greedy_result=None):
    print(f"Running algorithm with params: m={m}, n={n}, k={k}, j={j}, s={s}, min_cover={min_cover}, opt_level={optimization_level}")

    if len(selected_numbers) < k:
        print(f"Error: Selected numbers count ({len(selected_numbers)}) is less than k ({k})")
        return [], True

    # 解析 required_cover
    required_cover = resolve_required_cover(j, s, min_cover)
    
    if optimization_level == 1:
        bt_time_limit = 20.0
    elif optimization_level == 2:
        bt_time_limit = 40.0
    else:
        bt_time_limit = 70.0

    # 智能路由：如果是单覆盖并且 C++ 可用，我们可以使用 Beam Search 混合模式
    if required_cover == 1 and _CPP_AVAILABLE and _CPP_ULTRA_ENABLED:
        # 如果是极小规模，DFS 能秒出精确解，直接走原有 DFS 流程
        if n <= 12:
            return run_backtracking_pruning(
                m, n, k, j, s, min_cover, selected_numbers,
                progress_callback=progress_callback,
                greedy_result=initial_greedy_result,
                emit_greedy_progress=(initial_greedy_result is None),
                time_budget_seconds=bt_time_limit,
            )
            
        # 中大规模走 Beam Search
        if optimization_level == 1:
            beam_width = 20
            expand_k = 5
            beam_time_limit = 20.0
        elif optimization_level == 2:
            beam_width = 60
            expand_k = 10
            beam_time_limit = 40.0
        else: # optimization_level >= 3
            beam_width = 100 if n <= 18 else 500
            expand_k = 15
            beam_time_limit = 70.0

        print(f"Active C++ Ultra Exact Solver (beam={beam_width}, expand_k={expand_k}, t={beam_time_limit}s)")
        try:
            # 报告初始进度
            if progress_callback and initial_greedy_result is None:
                # 复用外部已计算的 greedy 结果，避免重复计算
                greedy_result = initial_greedy_result
                if greedy_result is None:
                    greedy_result = get_heuristic_greedy_selection(selected_numbers, k, j, s, min_cover)
                progress_callback({"stage": "greedy", "best_size": len(greedy_result), "result": greedy_result})
            
            # 执行 Beam Search (已经全部被绑定到了 C++ dfs_ultra)
            bt_results, aborted = _cover_core.beam_search_selection(
                list(selected_numbers), k, j, s, beam_width, expand_k, beam_time_limit
            )
            
            if bt_results:
                return bt_results, aborted
        except Exception as e:
            print(f"C++ Engine failed: {e}. Falling back to standard backtracking.")
            # 出错回退
            pass

    # 默认流程（包含多覆盖或者 C++ 不可用，或者 fallback）
    return run_backtracking_pruning(
        m, n, k, j, s, min_cover, selected_numbers,
        progress_callback=progress_callback,
        greedy_result=initial_greedy_result,
        emit_greedy_progress=(initial_greedy_result is None),
        time_budget_seconds=bt_time_limit,
    )
