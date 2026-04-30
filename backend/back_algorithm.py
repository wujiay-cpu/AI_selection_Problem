import bisect
import heapq
import itertools
import math
import time


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

    if required_cover == 1:
        all_targets, sub_to_target_mask = build_targets_and_subset_inverted_index(sample_pool, j, s)
        target_count = len(all_targets)
        candidates = build_unique_candidates_bitmask(sample_pool, k, s, sub_to_target_mask)

        n = len(sample_pool)
        all_mask = (1 << target_count) - 1

        # ---- 改动1：构建目标稀缺度权重 ----
        # 统计每个目标被多少个候选覆盖（频率），频率越低权重越高
        target_freq = [0] * target_count
        
        # 使用 numpy 或直接 Python 快速加和
        # 由于这里是在 Python 层，我们用快速的掩码位测试代替 while bit 循环
        # (1 << i) 测试每个 target 是否存在
        for i in range(target_count):
            target_bit = 1 << i
            target_freq[i] = sum(1 for _, cmask, _ in candidates if cmask & target_bit)
            
        # 权重 = 1 / 频率，最小按 1 处理避免除零
        target_weight = [1.0 / max(f, 1) for f in target_freq]

        # 预计算每个候选的加权得分（基于所有目标，用于 tie-break）
        cand_weighted_score = []
        for _, cmask, _ in candidates:
            score = 0.0
            for i in range(target_count):
                if (cmask >> i) & 1:
                    score += target_weight[i]
            cand_weighted_score.append(score)
        # ---- 改动1 end ----

        if n >= 21 and len(candidates) > 20000:
            top_ids = {i for i, _ in heapq.nlargest(20000, enumerate(candidates), key=lambda x: x[1][2])}
            phase1 = [i for i in range(len(candidates)) if i in top_ids]
            phase2 = [i for i in range(len(candidates)) if i not in top_ids]
        else:
            phase1 = list(range(len(candidates)))
            phase2 = []

        # ---- 改动1：加权增益堆，tie-break 使用稀缺度加权得分 ----
        # 移除高频调用的 compute_weighted_gain，直接使用静态的 cand_weighted_score 进行 tie-break。
        # 这样堆内每次只需比较原生的 bit_count()，不再有纯 Python 的 bit 循环。

        def run_phase(indexes, covered_mask, remaining, selected_ids):
            if remaining <= 0 or not indexes:
                return covered_mask, remaining
            # 堆元素：(-raw_gain, -weighted_gain, i)
            # 优先按 raw_gain 排序，相同时用静态预计算的 weighted_gain tie-break
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
                    # 增益已变化，重新入堆（继续使用静态加权得分做 tie-break，无额外开销）
                    heapq.heappush(heap, (-real_gain, -cand_weighted_score[i], i))
                    continue
                active[i] = False
                selected_ids.append(i)
                covered_mask |= add_mask
                remaining -= real_gain
            return covered_mask, remaining
        # ---- 改动1 end ----

        def prune_redundant(selected_ids):
            while True:
                to_remove = -1
                m = len(selected_ids)
                if m <= 1:
                    break

                # 预计算全集掩码
                current_full_mask = 0
                for cid in selected_ids:
                    current_full_mask |= candidates[cid][1]

                for i in range(m):
                    # 检查剔除第 i 个元素后是否依然覆盖全集
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

        # ---- 改动2：2-opt 局部替换（用1个组合替换2个组合）----
        def try_replace_2for1(selected_ids):
            """
            遍历已选组合中所有二元对 (i, j)，
            检查是否存在单个候选能覆盖移除它们后产生的空缺。
            优化：预计算 prefix/suffix 掩码将 O(m³) 降至 O(m²)，缓存 selected_set，
            并利用预分组索引减少扫描范围。
            """
            changed = True
            while changed:
                changed = False
                m = len(selected_ids)
                if m < 2:
                    break

                # 预计算所选元素的掩码数组
                masks = [candidates[cid][1] for cid in selected_ids]
                
                # 预计算前缀和后缀覆盖掩码
                prefix = [0] * m
                suffix = [0] * m
                curr = 0
                for i in range(m):
                    prefix[i] = curr
                    curr |= masks[i]
                curr = 0
                for i in range(m - 1, -1, -1):
                    suffix[i] = curr
                    curr |= masks[i]
                    
                full_mask = prefix[-1] | masks[-1]

                selected_set = set(selected_ids)
                
                for ii in range(m):
                    for jj in range(ii + 1, m):
                        # 利用前后缀以 O(1) 计算除去 ii 和 jj 后的剩余掩码
                        # 分三段：[0, ii-1], [ii+1, jj-1], [jj+1, m-1]
                        rest_mask = prefix[ii] | suffix[jj]
                        # 对于中间那一段，用位运算补充（由于是位运算，稍微循环一下也极快）
                        for kk in range(ii + 1, jj):
                            rest_mask |= masks[kk]

                        # 需要被补回来的目标
                        gap_mask = all_mask & ~rest_mask

                        if gap_mask == 0:
                            # 即使移除两个也能覆盖全集，直接删掉两个
                            new_ids = [cid for kk, cid in enumerate(selected_ids) if kk != ii and kk != jj]
                            selected_ids = new_ids
                            changed = True
                            break

                        # 寻找能单独覆盖 gap_mask 的候选
                        best_ci = -1
                        # 只有当候选覆盖的比特数 >= gap_mask 覆盖的比特数时，才可能包含 gap_mask
                        # 因此可以通过 candidates 预先排序好的特性（通常不需要）或者直接遍历
                        for ci in range(len(candidates)):
                            if ci in selected_set:
                                continue
                            if (candidates[ci][1] & gap_mask) == gap_mask:
                                best_ci = ci
                                break  # 找到第一个满足的即可

                        if best_ci != -1:
                            new_ids = [cid for kk, cid in enumerate(selected_ids) if kk != ii and kk != jj]
                            new_ids.append(best_ci)
                            selected_ids = new_ids
                            changed = True
                            break  # 重新开始外层循环

                    if changed:
                        break

            return selected_ids
        # ---- 改动2 end ----

        remaining = target_count
        covered_mask = 0
        selected_ids = []
        print(f"开始计算... 共有 {target_count} 个目标需要覆盖")

        covered_mask, remaining = run_phase(phase1, covered_mask, remaining, selected_ids)
        if remaining > 0:
            covered_mask, remaining = run_phase(phase2, covered_mask, remaining, selected_ids)

        if remaining == 0:
            selected_ids = prune_redundant(selected_ids)
            # ---- 改动2：贪心+冗余删除后，执行 2-opt 替换 ----
            before_2opt = len(selected_ids)
            selected_ids = try_replace_2for1(selected_ids)
            # 2-opt 可能引入新冗余，再做一轮清理
            selected_ids = prune_redundant(selected_ids)
            after_2opt = len(selected_ids)
            if after_2opt < before_2opt:
                print(f"2-opt 替换优化：{before_2opt} -> {after_2opt} 组")
            # ---- 改动2 end ----
        else:
            print("警告：无法继续覆盖")

        final_result = [sorted(list(candidates[i][0])) for i in selected_ids]
        return final_result

    # ---- required_cover > 1 分支 ----
    all_targets = list(itertools.combinations(sample_pool, j))
    candidate_cover_maps = build_candidates_cover_maps(sample_pool, k, all_targets, s)

    final_result = []
    covered_subsets = [set() for _ in all_targets]
    print(f"开始计算... 共有 {len(all_targets)} 个目标需要覆盖")

    # ---- 改动1（multi）：构建目标稀缺度权重（用于 required_cover > 1 的加权贪心）----
    # 统计每个目标有多少候选可以覆盖它
    target_freq_multi = [0] * len(all_targets)
    for _, cover_map, _ in candidate_cover_maps:
        for idx in cover_map:
            target_freq_multi[idx] += 1
    target_weight_multi = [1.0 / max(f, 1) for f in target_freq_multi]
    # ---- 改动1（multi）end ----

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
        best_weighted_gain = -1.0  # tie-break 用

        for i, (_, cover_map, _) in enumerate(candidate_cover_maps):
            gain = 0
            weighted_gain = 0.0
            for idx, add_set in cover_map.items():
                before = min(required_cover, len(covered_subsets[idx]))
                after = min(required_cover, len(covered_subsets[idx].union(add_set)))
                delta = after - before
                gain += delta
                # ---- 改动1（multi）：加权增益 tie-break ----
                weighted_gain += delta * target_weight_multi[idx]
                # ---- 改动1（multi）end ----
            # 优先选 gain 最大，相同时选 weighted_gain 最大
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

    # ---- 改动3：required_cover > 1 分支补充冗余删除 ----
    # 原始代码在此分支没有任何后处理，这里加入冗余删除逻辑
    final_result = _prune_redundant_multi(final_result, sample_pool, k, all_targets, s, required_cover)
    # ---- 改动3 end ----

    return final_result


def _prune_redundant_multi(final_result, sample_pool, k, all_targets, s, required_cover):
    """
    改动3：针对 required_cover > 1 的冗余删除。
    优化：使用增量维护覆盖计数，将 O(m*T) 降至 O(当前候选覆盖的目标数)，极大幅度提高速度。
    """
    if not final_result:
        return final_result

    # 预计算每个候选的 cover_map
    def build_cover_map_for(cand):
        c_set = set(cand)
        cover_map = {}
        for idx, t in enumerate(all_targets):
            inter = c_set.intersection(t)
            if len(inter) >= s:
                cover_map[idx] = set(itertools.combinations(sorted(inter), s))
        return cover_map

    cover_maps = [build_cover_map_for(tuple(c)) for c in final_result]
    num_targets = len(all_targets)
    
    # 初始化全局覆盖状态，这里我们直接存储每个目标被哪些组合（具体是哪些大小为 s 的组合）覆盖了
    # covered_elements[idx] = 包含所有候选覆盖该目标的元素的并集
    # 这里由于要维护每个目标的元素个数，我们可以使用一个字典记录元素的引用计数
    global_coverage = [{} for _ in range(num_targets)]
    
    def add_coverage(cover_map):
        for idx, elements in cover_map.items():
            for elem in elements:
                global_coverage[idx][elem] = global_coverage[idx].get(elem, 0) + 1
                
    def remove_coverage(cover_map):
        for idx, elements in cover_map.items():
            for elem in elements:
                global_coverage[idx][elem] -= 1
                if global_coverage[idx][elem] == 0:
                    del global_coverage[idx][elem]

    # 初始化全局覆盖
    for cmap in cover_maps:
        add_coverage(cmap)

    changed = True
    while changed:
        changed = False
        m = len(final_result)
        if m <= 1:
            break
            
        for i in range(m):
            cmap = cover_maps[i]
            
            # 模拟移除第 i 个候选
            can_remove = True
            for idx, elements in cmap.items():
                # 计算移除后该目标还剩多少个不同的覆盖元素
                remaining_elements_count = len(global_coverage[idx])
                for elem in elements:
                    if global_coverage[idx][elem] == 1:
                        remaining_elements_count -= 1
                if remaining_elements_count < required_cover:
                    can_remove = False
                    break
                    
            if can_remove:
                # 确实可以移除，从全局覆盖中减去它的贡献
                remove_coverage(cmap)
                final_result.pop(i)
                cover_maps.pop(i)
                changed = True
                break  # 重新开始外层 while

    return final_result


def run_heuristic_greedy(m, n, k, j, s, min_cover, selected_numbers):
    """
    启发式贪心
    """
    return get_heuristic_greedy_selection(selected_numbers, k, j, s, min_cover)


def run_backtracking_pruning(m, n, k, j, s, min_cover, selected_numbers, progress_callback=None):
    """
    成员 C：先贪心给出结果，再在时限内尝试回溯改进
    """
    greedy_result = get_heuristic_greedy_selection(selected_numbers, k, j, s, min_cover)
    if progress_callback:
        progress_callback({"stage": "greedy", "best_size": len(greedy_result), "result": greedy_result})
    
    meta = improve_with_backtracking_status(selected_numbers, k, j, s, min_cover, greedy_result, 120.0, progress_callback=progress_callback)
    improved = meta.get("result", [])
    aborted = meta.get("aborted", False)
    
    if improved and len(improved) < len(greedy_result):
        print(f"回溯改进成功：{len(greedy_result)} -> {len(improved)} 组")
        return improved, aborted
    return greedy_result, aborted


def improve_with_backtracking_status(sample_pool, k, j, s, min_cover, greedy_result, time_budget_seconds=120.0, progress_callback=None):
    if not greedy_result:
        return {"result": [], "aborted": False, "proved_optimal": False}
    upper_bound_groups = len(greedy_result) - 1
    if upper_bound_groups <= 0:
        return {"result": [], "aborted": False, "proved_optimal": True}

    meta = get_backtracking_selection(
        sample_pool,
        k,
        j,
        s,
        min_cover,
        max_seconds=time_budget_seconds,
        upper_bound_groups=upper_bound_groups,
        fallback_to_greedy=False,
        return_meta=True,
        progress_callback=progress_callback,
    )
    return {
        "result": meta.get("result", []),
        "aborted": meta.get("aborted", False),
        "proved_optimal": (not meta.get("aborted", False)) and (not meta.get("result")),
    }


def improve_with_backtracking(sample_pool, k, j, s, min_cover, greedy_result, time_budget_seconds=120.0, progress_callback=None):
    return improve_with_backtracking_status(sample_pool, k, j, s, min_cover, greedy_result, time_budget_seconds, progress_callback=progress_callback).get("result", [])


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

def get_backtracking_selection_bitmask(sample_pool, k, j, s, max_ops, max_seconds, upper_bound_groups=None, fallback_to_greedy=True, return_meta=False, progress_callback=None):
    all_targets, sub_to_target_mask = build_targets_and_subset_inverted_index(sample_pool, j, s)
    target_count = len(all_targets)
    all_mask = (1 << target_count) - 1
    
    bitmask_candidates = build_unique_candidates_bitmask(sample_pool, k, s, sub_to_target_mask)
    
    # ---- 根对称性破缺：canonical c_1 放到最前 ---- 
    canonical_c1 = tuple(sorted(sample_pool[:k]))
    
    canonical_item = None 
    other_items = [] 
    for cand, cmask, bc in bitmask_candidates: 
        if tuple(sorted(cand)) == canonical_c1 and canonical_item is None: 
            canonical_item = (cand, cmask, bc) 
        else: 
            other_items.append((cand, cmask, bc)) 
    
    # 直接使用已有的 bc (bit_count) 排序
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
    
    root_symmetry_broken = canonical_item is not None 
    if root_symmetry_broken: 
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
        "cut_deadline": 0,
        "cut_max_ops": 0,
        "cut_terminal": 0,
        "cut_future": 0,
        "cut_memo": 0,
        "cut_lb_single": 0,
        "nodes": 0,
    }

    future_masks = [0] * (candidate_count + 1)
    for i in range(candidate_count - 1, -1, -1):
        future_masks[i] = future_masks[i+1] | candidates[i][1]

    def feasible(start, covered_mask):
        return (covered_mask | future_masks[start]) == all_mask

    def dfs(start, covered_mask, left, picked):
        nonlocal answer, ops, aborted
        now = time.perf_counter()
        if aborted:
            return False
        if now >= deadline:
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

        # 投影到 uncovered 上做支配剪枝并过滤无增益候选
        # 为了修复 pivot+start 冲突导致的漏解问题，回归标准 start 遍历
        proj = [] 
        for ci in range(start, candidate_count): 
            pm = candidates[ci][1] & uncovered 
            if pm == 0: continue
            proj.append((pm.bit_count(), ci, pm)) 
        
        if not proj:
            return False

        proj.sort(key=lambda x: x[0], reverse=True) 
        
        # 一步下界
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
    print(f"开始回溯搜索 (位运算加速)... 共有 {target_count} 个目标，{candidate_count} 个有效候选组，运算上限 {ops_label}，时间上限 {max_seconds:.1f}s，下界LB={lower_bound}")
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
                progress_callback({"stage": "backtracking", "best_size": size, "result": best_answer, "elapsed": time.perf_counter() - start_time})
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
        return best_answer

    if aborted:
        print("回溯达到上限，停止搜索")
        print(f"调试统计: nodes={stats['nodes']}, cut_deadline={stats['cut_deadline']}, cut_max_ops={stats['cut_max_ops']}, cut_future={stats['cut_future']}, cut_memo={stats['cut_memo']}, cut_lb_single={stats['cut_lb_single']}")
        if fallback_to_greedy:
            result = get_heuristic_greedy_selection(sample_pool, k, j, s, 1)
            if return_meta:
                return {"result": result, "aborted": True}
            return result
        if return_meta:
            return {"result": [], "aborted": True}
        return []
    print(f"调试统计: nodes={stats['nodes']}, cut_deadline={stats['cut_deadline']}, cut_max_ops={stats['cut_max_ops']}, cut_future={stats['cut_future']}, cut_memo={stats['cut_memo']}, cut_lb_single={stats['cut_lb_single']}")
    if return_meta:
        return {"result": answer, "aborted": False}
    return answer


def get_backtracking_selection(sample_pool, k, j, s, min_cover=1, max_seconds=20.0, upper_bound_groups=None, fallback_to_greedy=True, return_meta=False, progress_callback=None):
    n = len(sample_pool)
    need_cover = resolve_required_cover(j, s, min_cover)
    max_ops = get_backtracking_operation_limit(n, need_cover)

    if need_cover == 1:
        return get_backtracking_selection_bitmask(sample_pool, k, j, s, max_ops, max_seconds, upper_bound_groups, fallback_to_greedy, return_meta, progress_callback=progress_callback)

    targets = list(itertools.combinations(sample_pool, j))
    candidates = build_candidates_cover_maps(sample_pool, k, targets, s)

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
    print(f"开始回溯搜索... 共有 {len(targets)} 个目标，{len(candidates)} 个有效候选组，运算上限 {ops_label}，时间上限 {max_seconds:.1f}s")
    for size in range(1, max_size + 1):
        if dfs(0, [set() for _ in range(target_count)], size, []):
            print(f"找到最优解，组数: {size}")
            if return_meta:
                return {"result": answer, "aborted": False}
            return answer
    if return_meta:
        return {"result": answer, "aborted": aborted}
    return answer


def run_algorithm(m, n, k, j, s, min_cover, selected_numbers, progress_callback=None):
   
    print(f"Running algorithm with params: m={m}, n={n}, k={k}, j={j}, s={s}, min_cover={min_cover}")

    if len(selected_numbers) < k:
        print(f"Error: Selected numbers count ({len(selected_numbers)}) is less than k ({k})")
        return [], True

    return run_backtracking_pruning(m, n, k, j, s, min_cover, selected_numbers, progress_callback=progress_callback)