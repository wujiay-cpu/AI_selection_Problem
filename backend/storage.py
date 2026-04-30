import os
import json
import glob

DATA_DIR = "results"

def ensure_data_dir():
    if not os.path.exists(DATA_DIR):
        os.makedirs(DATA_DIR)

def get_next_run_count(m, n, k, j, s):
    """
    Determines the next run count for the given parameters by scanning existing files.
    Filename format: m-n-k-j-s-runCount-resultCount.json
    """
    ensure_data_dir()
    pattern = os.path.join(DATA_DIR, f"{m}-{n}-{k}-{j}-{s}-*-*.json")
    files = glob.glob(pattern)
    
    max_run = 0
    for file_path in files:
        filename = os.path.basename(file_path)
        try:
            parts = filename.replace(".json", "").split("-")
            # Format: m-n-k-j-s-runCount-resultCount
            if len(parts) >= 7: # Expecting 7 parts: m, n, k, j, s, run, result
                run_count = int(parts[5])
                if run_count > max_run:
                    max_run = run_count
        except (ValueError, IndexError):
            continue
            
    return max_run + 1

def save_result(m, n, k, j, s, min_cover, algorithm_type, selected_numbers, result_combinations):
    """
    Saves the result to a file.
    Returns the filename saved.
    """
    ensure_data_dir()
    run_count = get_next_run_count(m, n, k, j, s)
    result_count = len(result_combinations)
    
    filename = f"{m}-{n}-{k}-{j}-{s}-{run_count}-{result_count}.json"
    filepath = os.path.join(DATA_DIR, filename)
    
    data = {
        "params": {
            "m": m, "n": n, "k": k, "j": j, "s": s, "min_cover": min_cover, "algorithm": algorithm_type
        },
        "selected_numbers": selected_numbers,
        "combinations": result_combinations
    }
    
    with open(filepath, 'w') as f:
        json.dump(data, f, indent=4)
        
    return filename

def list_results():
    """
    Returns a list of all result filenames.
    """
    ensure_data_dir()
    files = glob.glob(os.path.join(DATA_DIR, "*.json"))
    return [os.path.basename(f) for f in files]

def load_result(filename):
    """
    Loads the content of a result file.
    """
    filepath = os.path.join(DATA_DIR, filename)
    if os.path.exists(filepath):
        with open(filepath, 'r') as f:
            return json.load(f)
    return None

def delete_result(filename):
    """
    Deletes a result file.
    """
    filepath = os.path.join(DATA_DIR, filename)
    if os.path.exists(filepath):
        os.remove(filepath)
        return True
    return False
