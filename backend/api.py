import asyncio
import json
from queue import Queue
from threading import Thread
from typing import List, Union, Literal, Optional

from fastapi import FastAPI, HTTPException
from fastapi.responses import StreamingResponse
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

from .algorithm import run_algorithm, run_heuristic_greedy
from .storage import save_result, list_results, load_result, delete_result

app = FastAPI(title="Selection System API")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

class ComputeRequest(BaseModel):
    m: int
    n: int
    k: int
    j: int
    s: int
    min_cover: Union[int, str] = 1
    selected_numbers: List[int] = Field(min_length=1)
    algorithm: Literal["backtracking_pruning", "heuristic_greedy"] = "backtracking_pruning"
    save: bool = False

class StoreRequest(BaseModel):
    m: int
    n: int
    k: int
    j: int
    s: int
    min_cover: Union[int, str] = 1
    algorithm: str
    selected_numbers: List[int] = Field(min_length=1)
    combinations: List[List[int]]

@app.get("/api/health")
def health():
    return {"ok": True}

async def stream_backtracking_results(req: ComputeRequest):
    q = Queue()

    def progress_callback(data):
        q.put(json.dumps(data))

    def run_in_thread():
        try:
            greedy_combinations = run_heuristic_greedy(
                req.m, req.n, req.k, req.j, req.s, req.min_cover, req.selected_numbers
            )
            q.put(json.dumps({"stage": "greedy", "result": greedy_combinations}))

            # unpack the result correctly
            result, aborted = run_algorithm(
                req.m, req.n, req.k, req.j, req.s, req.min_cover, req.selected_numbers, progress_callback=progress_callback
            )
            q.put(json.dumps({"stage": "completed", "result": result, "aborted": aborted}))
        except Exception as e:
            q.put(json.dumps({"stage": "error", "detail": str(e)}))
        finally:
            q.put(None) # End of stream signal

    thread = Thread(target=run_in_thread)
    thread.start()

    while True:
        item = await asyncio.to_thread(q.get)
        if item is None:
            break
        yield f"data: {item}\n\n"

@app.post("/api/compute")
async def compute(req: ComputeRequest):
    if len(req.selected_numbers) < req.k:
        raise HTTPException(status_code=400, detail="selected_numbers length cannot be less than k")

    if req.algorithm == "heuristic_greedy":
        combinations = run_heuristic_greedy(
            req.m, req.n, req.k, req.j, req.s, req.min_cover, req.selected_numbers
        )
        return {
            "filename": None, # Greedy does not save by default
            "count": len(combinations),
            "combinations": combinations,
        }
    
    # For backtracking, use streaming
    return StreamingResponse(stream_backtracking_results(req), media_type="text/event-stream")

@app.post("/api/store")
def store(req: StoreRequest):
    filename = save_result(
        req.m,
        req.n,
        req.k,
        req.j,
        req.s,
        req.min_cover,
        req.algorithm,
        req.selected_numbers,
        req.combinations,
    )
    return {"filename": filename}

import os

@app.get("/api/results")
def results():
    def sort_key(filename):
        try:
            name_without_ext = filename.split(".json")[0]
            return [int(p) for p in name_without_ext.split("-")]
        except Exception:
            return [0]

    files = list_results()
    files.sort(key=sort_key, reverse=False)
    
    return {"files": files}

@app.get("/api/results/{filename}")
def result_detail(filename: str):
    data = load_result(filename)
    if data is None:
        raise HTTPException(status_code=404, detail="File not found")
    return data

@app.delete("/api/results/{filename}")
def result_delete(filename: str):
    ok = delete_result(filename)
    if not ok:
        raise HTTPException(status_code=404, detail="File not found")
    return {"deleted": True}