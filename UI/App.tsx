/**
 * @license
 * SPDX-License-Identifier: Apache-2.0
 */

import React, { useState, useEffect, useMemo, useRef } from 'react';
import { 
  Play, 
  Save, 
  Trash2, 
  RefreshCw, 
  ChevronRight, 
  Terminal, 
  History, 
  Settings2, 
  Database,
  Cpu,
  Info,
  CheckCircle2,
  AlertCircle,
  Clock,
  LayoutGrid,
  FileText,
  Printer
} from 'lucide-react';
import { motion, AnimatePresence } from 'motion/react';

// --- Types ---

interface StoredResultData {
  params: {
    m: number;
    n: number;
    k: number;
    j: number;
    s: number;
    min_cover: number | 'all';
    algorithm: string;
  };
  selected_numbers: number[];
  combinations: number[][];
}

// --- Utils ---

const generateCombinations = (pool: number[], k: number): number[][] => {
  const results: number[][] = [];
  const combine = (start: number, current: number[]) => {
    if (current.length === k) {
      results.push([...current]);
      return;
    }
    for (let i = start; i < pool.length; i++) {
      current.push(pool[i]);
      combine(i + 1, current);
      current.pop();
    }
  };
  combine(0, []);
  return results;
};

// --- Styles ---
const scrollbarStyles = `
  .custom-scrollbar::-webkit-scrollbar {
    width: 8px;
    height: 8px;
  }
  .custom-scrollbar::-webkit-scrollbar-track {
    background: rgba(0, 0, 0, 0.1);
    border-radius: 4px;
  }
  .custom-scrollbar::-webkit-scrollbar-thumb {
    background: rgba(255, 255, 255, 0.15);
    border-radius: 4px;
    border: 2px solid transparent;
    background-clip: content-box;
  }
  .custom-scrollbar::-webkit-scrollbar-thumb:hover {
    background: rgba(255, 255, 255, 0.25);
    border: 2px solid transparent;
    background-clip: content-box;
  }
`;

export default function App() {
  // --- State ---
  const [activeTab, setActiveTab] = useState<'generator' | 'history'>('generator');
  
  // Params
  const [m, setM] = useState('');
  const [n, setN] = useState('');
  const [k, setK] = useState('');
  const [j, setJ] = useState('');
  const [s, setS] = useState('');
  const [minCover, setMinCover] = useState('');
  const [optimizationLevel, setOptimizationLevel] = useState<number>(2);
  const [selectionMode, setSelectionMode] = useState<'random' | 'manual'>('random');
  const [manualInput, setManualInput] = useState('');

  // Results
  const [isRunning, setIsRunning] = useState(false);
  const [status, setStatus] = useState<'idle' | 'running' | 'success' | 'warning' | 'error'>('idle');
  const [isOptimal, setIsOptimal] = useState<boolean | null>(null);
  const [currentResults, setCurrentResults] = useState<number[][]>([]);
  const [executionTime, setExecutionTime] = useState(0);
  const [selectedIndex, setSelectedIndex] = useState(-1);
  const [logs, setLogs] = useState<string[]>([]);
  const [currentSelectedNumbers, setCurrentSelectedNumbers] = useState<number[]>([]);
  const [currentAlgorithm, setCurrentAlgorithm] = useState('backtracking_pruning');

  // History
  const [historyFiles, setHistoryFiles] = useState<string[]>([]);
  const [selectedHistoryFile, setSelectedHistoryFile] = useState<string | null>(null);
  const [selectedHistoryData, setSelectedHistoryData] = useState<StoredResultData | null>(null);
  const [historySelectedIndex, setHistorySelectedIndex] = useState(-1);

  const terminalRef = useRef<HTMLDivElement>(null);
  const historyRef = useRef<HTMLDivElement>(null);
  const abortControllerRef = useRef<AbortController | null>(null);
  const scrollDirectionRef = useRef<'top' | 'bottom'>('bottom');

  // --- Effects ---
  useEffect(() => {
    refreshHistoryList();
  }, []);

  // Auto scroll: scroll to top when backtracking improves to show new results, otherwise scroll to bottom
  useEffect(() => {
    if (terminalRef.current) {
      if (scrollDirectionRef.current === 'top') {
        terminalRef.current.scrollTop = 0;
      } else {
        terminalRef.current.scrollTop = terminalRef.current.scrollHeight;
      }
      scrollDirectionRef.current = 'bottom';
    }
  }, [logs]);

  const isMinCoverLocked = s !== '' && j !== '' && s === j;

  useEffect(() => {
    if (isMinCoverLocked) {
      setMinCover('all');
    } else if (minCover === 'all') {
      setMinCover('');
    }
  }, [isMinCoverLocked, minCover]);

  const addLog = (msg: string) => setLogs(prev => [...prev, msg]);

  const setResultBlock = (
    duration: number,
    M: number,
    N: number,
    K: number,
    J: number,
    S: number,
    minCoverValue: number | 'all',
    algorithmLabel: string,
    selectedNumbers: number[],
    results: number[][],
  ) => {
    const lines = [
      `Execution Time: ${duration.toFixed(3)} seconds`,
      `Params: m=${M}, n=${N}, k=${K}, j=${J}, s=${S}, min_cover=${minCoverValue}, algorithm=${algorithmLabel}`,
      `Selected Numbers: [${selectedNumbers.join(', ')}]`,
      `Result: Found ${results.length} groups of k=${K} samples`,
      '-'.repeat(60),
      ...results.map((combo, idx) => `#${String(idx + 1).padStart(3, '0')}: [${combo.join(', ')}]`),
    ];
    setLogs(lines);
  };

  const handleNext = () => {
    if (currentResults.length === 0) return;
    const nextIdx = (selectedIndex + 1) % currentResults.length;
    setSelectedIndex(nextIdx);
    
    // Logic: scroll to corresponding log line (optional enhancement)
  };

  const runAlgorithm = async () => {
    setIsRunning(true);
    setStatus('running');
    setLogs(['Calculating... please wait.']);
    setSelectedIndex(-1);
    setIsOptimal(null);

    try {
      const M = parseInt(m, 10);
      const N = parseInt(n, 10);
      const K = parseInt(k, 10);
      const J = parseInt(j, 10);
      const S = parseInt(s, 10);
      if ([M, N, K, J, S].some(v => Number.isNaN(v))) throw new Error('Please fill m, n, k, j, s with valid numbers');
      if (!(3 <= S && S <= 7)) throw new Error('s must be between 3 and 7');
      if (!(S <= J && J <= K)) throw new Error('Constraint s <= j <= k failed');

      const minCoverValue: number | 'all' = S === J ? 'all' : ((minCover || '').trim() ? parseInt(minCover, 10) : 1);

      let samples: number[] = [];
      if (selectionMode === 'random') {
        const pool = Array.from({ length: M }, (_, i) => i + 1);
        for (let i = 0; i < N; i++) {
          const idx = Math.floor(Math.random() * pool.length);
          samples.push(pool.splice(idx, 1)[0]);
        }
      } else {
        const parsed = manualInput
          .split(/[\s,]+/)
          .map((x) => Number.parseInt(x, 10))
          .filter((x): x is number => Number.isFinite(x));
        samples = Array.from(new Set<number>(parsed)).sort((a, b) => a - b);
        if (samples.length !== N) throw new Error(`Need exactly ${N} unique numbers`);
      }
      samples = [...new Set(samples)].sort((a, b) => a - b);

      const controller = new AbortController();
      abortControllerRef.current = controller;

      try {
        const btStart = performance.now();
        const btResp = await fetch('http://127.0.0.1:8000/api/compute', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            m: M,
            n: N,
            k: K,
            j: J,
            s: S,
            min_cover: minCoverValue,
            selected_numbers: samples,
            algorithm: 'backtracking_pruning',
            optimization_level: optimizationLevel,
            save: false,
          }),
          signal: controller.signal,
        });

        if (!btResp.ok || !btResp.body) {
          throw new Error('Backtracking stream failed to start.');
        }

        const reader = btResp.body.getReader();
        const decoder = new TextDecoder();
        let buffer = '';
        let finalResultReceived = false;
        let greedyResults: number[][] = [];
        let greedyDuration = 0;
        let lastResultCount = Infinity;

        // Client-side timeout fallback: if 'completed' is not received within 90s, force cancel the stream
        const CLIENT_TIMEOUT_MS = 90 * 1000;
        let clientTimedOut = false;
        const clientTimeoutId = setTimeout(() => {
          clientTimedOut = true;
          reader.cancel();
        }, CLIENT_TIMEOUT_MS);

        while (true) {
          const { done, value } = await reader.read();
          if (done) {
            break;
          }

          buffer += decoder.decode(value, { stream: true });
          const messages = buffer.split('\n\n');
          buffer = messages.pop() || '';

          for (const message of messages) {
            if (message.startsWith('data: ')) {
              try {
                const data = JSON.parse(message.substring(6));
                const btDuration = (performance.now() - btStart) / 1000;

                if (data.stage === 'greedy') {
                  greedyResults = data.result || [];
                  greedyDuration = (performance.now() - btStart) / 1000;
                  lastResultCount = greedyResults.length;
                  setResultBlock(greedyDuration, M, N, K, J, S, minCoverValue, 'heuristic_greedy (preview)', samples, greedyResults);
                  setCurrentResults(greedyResults);
                  setCurrentSelectedNumbers(samples);
                  setCurrentAlgorithm('heuristic_greedy (preview)');
                  setExecutionTime(greedyDuration);
                } else if (data.stage === 'backtracking_started') {
                  const targetCount = data.greedy_size ?? lastResultCount;
                  addLog(`Backtracking search started (up to 90s). Searching for a result below ${targetCount} groups...`);
                } else if (data.stage === 'backtracking' && data.result) {
                  const btResults: number[][] = data.result;
                  const alg = 'backtracking_pruning';
                  // Build log with improvement notice at the top so user sees it immediately
                  const improvedLines = [
                    `★ IMPROVED: from ${greedyResults.length} to ${btResults.length} groups (+${(btDuration - greedyDuration).toFixed(2)}s backtracking)`,
                    '─'.repeat(60),
                    `Execution Time: ${btDuration.toFixed(3)} seconds`,
                    `Params: m=${M}, n=${N}, k=${K}, j=${J}, s=${S}, min_cover=${minCoverValue}, algorithm=${alg}`,
                    `Selected Numbers: [${samples.join(', ')}]`,
                    `Result: Found ${btResults.length} groups of k=${K} samples`,
                    '─'.repeat(60),
                    ...btResults.map((combo, idx) => `#${String(idx + 1).padStart(3, '0')}: [${combo.join(', ')}]`),
                  ];
                  lastResultCount = btResults.length;
                  scrollDirectionRef.current = 'top'; // scroll to top to show the IMPROVED header
                  setLogs(improvedLines);
                  setCurrentResults(btResults);
                  setCurrentAlgorithm(alg);
                  setExecutionTime(btDuration);
                  setIsOptimal(true); // Found improvement -> Green
                } else if (data.stage === 'completed') {
                  finalResultReceived = true;
                  const finalResult: number[][] = data.result || [];
                  const aborted = data.aborted;
                  const btDuration = (performance.now() - btStart) / 1000;

                  if (finalResult.length > 0 && finalResult.length < lastResultCount) {
                    const alg = 'backtracking_pruning';
                    const improvedLines = [
                      `★ IMPROVED: ${lastResultCount} groups → ${finalResult.length} groups  (+${(btDuration - greedyDuration).toFixed(2)}s backtracking)`,
                      '─'.repeat(60),
                      `Execution Time: ${btDuration.toFixed(3)} seconds`,
                      `Params: m=${M}, n=${N}, k=${K}, j=${J}, s=${S}, min_cover=${minCoverValue}, algorithm=${alg}`,
                      `Selected Numbers: [${samples.join(', ')}]`,
                      `Result: Found ${finalResult.length} groups of k=${K} samples`,
                      '─'.repeat(60),
                      ...finalResult.map((combo, idx) => `#${String(idx + 1).padStart(3, '0')}: [${combo.join(', ')}]`),
                    ];
                    scrollDirectionRef.current = 'top';
                    setLogs(improvedLines);
                    setCurrentResults(finalResult);
                    setCurrentAlgorithm(alg);
                    setExecutionTime(btDuration);
                  }

                  // Stricter timeout detection: backend returns aborted OR running time > 89s with no improvement
                  const isTimedOut = aborted === true || (btDuration > 89.0 && finalResult.length >= greedyResults.length);

                  if (isTimedOut) {
                    if (finalResult.length < greedyResults.length) {
                      addLog(`Backtracking found improvements but reached time limit.`);
                      setStatus('success');
                      setIsOptimal(true);
                    } else {
                      addLog(`Backtracking reached time limit without finding better result.`);
                      setStatus('warning');
                      setIsOptimal(false);
                    }
                  } else {
                    addLog(`Backtracking search completed.`);
                    setStatus('success');
                    setIsOptimal(true);
                  }
                } else if (data.stage === 'error') {
                  throw new Error(data.detail || 'Error in backtracking stream');
                }
              } catch (e) {
                console.error('Error parsing stream data:', e);
                addLog(`Error processing update: ${e}`);
              }
            }
          }
        }

        clearTimeout(clientTimeoutId);

        if (clientTimedOut && !finalResultReceived) {
          addLog(`Backtracking reached time limit without finding better result.`);
          setStatus('warning');
          setIsOptimal(false);
        } else if (!finalResultReceived) {
          addLog('Backtracking stream ended unexpectedly. Showing best available result.');
          setStatus('warning');
          setIsOptimal(false);
        }

      } catch (btErr: any) {
        if ((btErr as any)?.name === 'AbortError') return;
        addLog('');
        addLog(`Backtracking failed. Keeping greedy result. (${btErr?.message || 'unknown error'})`);
        setStatus('warning');
        setIsOptimal(false);
      }
    } catch (err: any) {
      if ((err as any)?.name === 'AbortError') return;
      alert(`Error: ${err?.message || 'unknown error'}`);
      setStatus('error');
    } finally {
      abortControllerRef.current = null;
      setIsRunning(false);
    }
  };

  const saveToHistory = async () => {
    if (currentResults.length === 0) return alert('No results to store.');
    try {
      const M = parseInt(m, 10), N = parseInt(n, 10), K = parseInt(k, 10), J = parseInt(j, 10), S = parseInt(s, 10);
      const minCoverValue = S === J ? 'all' : (parseInt(minCover, 10) || 1);
      const resp = await fetch('http://127.0.0.1:8000/api/store', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ m: M, n: N, k: K, j: J, s: S, min_cover: minCoverValue, algorithm: currentAlgorithm, selected_numbers: currentSelectedNumbers, combinations: currentResults }),
      });
      if (!resp.ok) throw new Error('Store failed');
      await refreshHistoryList();
      alert('Stored successfully');
    } catch (e: any) { alert(e.message); }
  };

  const clearAll = () => {
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
      abortControllerRef.current = null;
    }
    setCurrentResults([]);
    setLogs([]);
    setStatus('idle');
    setSelectedIndex(-1);
    setIsRunning(false);
    setOptimizationLevel(2);
    setM(''); setN(''); setK(''); setJ(''); setS(''); setMinCover(''); setManualInput('');
  };

  const refreshHistoryList = async () => {
    try {
      const resp = await fetch('http://127.0.0.1:8000/api/results');
      const data = await resp.json();
      let files = Array.isArray(data?.files) ? data.files : [];

      // Frontend numeric sorting logic (Double Check)
      files.sort((a: string, b: string) => {
        const aParts = a.replace('.json', '').split('-').map(Number);
        const bParts = b.replace('.json', '').split('-').map(Number);
        
        // Compare numbers bit by bit
        for (let i = 0; i < Math.max(aParts.length, bParts.length); i++) {
          const numA = aParts[i] || 0;
          const numB = bParts[i] || 0;
          if (numA !== numB) return numA - numB;
        }
        return 0;
      });

      setHistoryFiles(files);
    } catch (e) { console.error(e); }
  };

  const deleteSelectedHistory = async () => {
    if (!selectedHistoryFile) return;
    if (!confirm(`Delete ${selectedHistoryFile}?`)) return;
    await fetch(`http://127.0.0.1:8000/api/results/${encodeURIComponent(selectedHistoryFile)}`, { method: 'DELETE' });
    setSelectedHistoryFile(null);
    setSelectedHistoryData(null);
    await refreshHistoryList();
  };

  return (
    <div className="h-screen w-full flex flex-col p-6 bg-bg-primary text-text-primary font-sans antialiased overflow-hidden">
      <style>{scrollbarStyles}</style>
      
      {/* Header */}
      <header className="flex justify-between items-center pb-4 border-b border-border shrink-0">
        <div>
          <h1 className="text-3xl font-bold tracking-tight text-text-primary">An Optimal Samples Selection System</h1>
        </div>
        <div className="flex bg-bg-secondary p-1 rounded-lg border border-border">
          <button 
            onClick={() => setActiveTab('generator')}
            className={`px-4 py-1.5 text-base font-medium rounded-md transition-all ${activeTab === 'generator' ? 'bg-border text-text-primary' : 'text-text-secondary hover:text-text-primary'} capitalize`}
          >
            Generator (S1)
          </button>
          <button 
            onClick={() => setActiveTab('history')}
            className={`px-4 py-1.5 text-base font-medium rounded-md transition-all ${activeTab === 'history' ? 'bg-border text-text-primary' : 'text-text-secondary hover:text-text-primary'} capitalize`}
          >
            History (S2)
          </button>
        </div>
      </header>

      {/* Main Content Area */}
      <main className="flex-grow min-h-0 pt-4">
        <AnimatePresence mode="wait">
          {activeTab === 'generator' ? (
            <motion.div 
              key="generator"
              initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}
              className="grid grid-cols-12 gap-4 h-full"
            >
              {/* Left Panel: Inputs */}
              <div className="col-span-4 flex flex-col space-y-4 min-h-0 overflow-y-auto custom-scrollbar pr-2">
                <section className="card p-4 shrink-0">
                  <h2 className="text-sm font-bold tracking-widest text-text-secondary mb-4 uppercase">Parameters Configuration</h2>
                  <div className="grid grid-cols-2 gap-x-4 gap-y-3">
                    {/* Params Inputs */}
                    {[
                      { label: 'm (Total 45-54)', val: m, set: setM },
                      { label: 'n (Sample 7-25)', val: n, set: setN },
                      { label: 'k (Group 4-7)', val: k, set: setK },
                      { label: 'j (Sample s≤j≤k)', val: j, set: setJ },
                      { label: 's (Covered 3≤s≤7)', val: s, set: setS },
                    ].map((item, idx) => (
                      <div key={idx} className="space-y-1">
                        <label className="text-xs font-bold text-text-secondary">{item.label}</label>
                        <input type="text" value={item.val} onChange={e => item.set(e.target.value)} className="input-field w-full text-base px-2 py-1" />
                      </div>
                    ))}
                    <div className="space-y-1">
                      <label className="text-xs font-bold text-text-secondary">at least (min_cover)</label>
                      <input 
                        type="text" value={minCover} 
                        onChange={e => setMinCover(e.target.value)} 
                        disabled={isMinCoverLocked} 
                        className={`input-field w-full text-base px-2 py-1 ${isMinCoverLocked ? 'opacity-50 cursor-not-allowed' : ''}`} 
                      />
                    </div>
                    <div className="space-y-1 col-span-2">
                      <label className="text-xs font-bold text-text-secondary">Optimization Level</label>
                      <div className="flex gap-3 text-xs">
                        <label className="flex items-center space-x-1 cursor-pointer">
                          <input
                            type="radio"
                            name="opt-level"
                            checked={optimizationLevel === 1}
                            onChange={() => setOptimizationLevel(1)}
                          />
                          <span>Fast</span>
                        </label>
                        <label className="flex items-center space-x-1 cursor-pointer">
                          <input
                            type="radio"
                            name="opt-level"
                            checked={optimizationLevel === 2}
                            onChange={() => setOptimizationLevel(2)}
                          />
                          <span>Standard</span>
                        </label>
                        <label className="flex items-center space-x-1 cursor-pointer">
                          <input
                            type="radio"
                            name="opt-level"
                            checked={optimizationLevel === 3}
                            onChange={() => setOptimizationLevel(3)}
                          />
                          <span>Deep</span>
                        </label>
                      </div>
                    </div>
                  </div>
                </section>

                <section className="card p-4 shrink-0">
                  <h2 className="text-sm font-bold tracking-widest text-text-secondary mb-4 uppercase">Sample Selection</h2>
                  <div className="flex space-x-6 mb-4">
                    <label className="flex items-center text-xs font-bold space-x-2 cursor-pointer">
                      <input type="radio" name="mode" checked={selectionMode === 'random'} onChange={() => setSelectionMode('random')} className="text-accent-green focus:ring-accent-green" />
                      <span>Random Selection</span>
                    </label>
                    <label className="flex items-center text-xs font-bold space-x-2 cursor-pointer">
                      <input type="radio" checked={selectionMode === 'manual'} onChange={() => setSelectionMode('manual')} className="accent-emerald-500" />
                      <span>Manual</span>
                    </label>
                  </div>
                  <textarea 
                    placeholder="e.g. 1 5 12 43 18" 
                    value={manualInput} 
                    onChange={e => setManualInput(e.target.value)} 
                    disabled={selectionMode === 'random'} 
                    className={`input-field h-24 w-full text-sm font-mono p-2 ${selectionMode === 'random' ? 'opacity-30' : ''}`}
                  />
                  {selectionMode === 'manual' && (
                    <p className="text-sm text-text-secondary mb-1">
                      Need <span className="text-text-primary font-bold">{n || 'n'}</span> unique integers from 1 to <span className="text-text-primary font-bold">{m || 'm'}</span>, space- or comma-separated.
                    </p>
                  )}
                </section>

                <section className="grid grid-cols-2 gap-2 shrink-0 pb-4">
                  <button onClick={runAlgorithm} disabled={isRunning} className="btn-base bg-emerald-600 hover:bg-emerald-500 h-12 text-sm font-bold uppercase tracking-wider">
                    {isRunning ? 'Computing...' : 'Run Algorithm'}
                  </button>
                  <button onClick={clearAll} className="btn-base bg-btn-bg hover:bg-btn-hover h-12 text-sm font-bold uppercase tracking-wider">Clear</button>
                  <button onClick={saveToHistory} className="btn-base bg-btn-bg hover:bg-btn-hover h-12 text-sm font-bold uppercase tracking-wider">Store</button>
                  <button onClick={() => window.print()} className="btn-base bg-btn-bg hover:bg-btn-hover h-12 text-sm font-bold uppercase tracking-wider">Print</button>
                </section>
              </div>

              {/* Right Panel: Output with Scroll */}
              <div className="col-span-8 flex flex-col min-h-0 h-full">
                <section className="card flex-grow flex flex-col min-h-0">
                  <div className="flex items-center justify-between p-3 border-b border-border bg-bg-accent shrink-0">
                    <div className="flex items-center space-x-3">
                      <span className="text-xs font-bold tracking-widest text-text-secondary uppercase">Results Output</span>
                      <div className="flex items-center space-x-2 px-2 py-0.5 rounded bg-bg-primary border border-border-input">
                        <div className={`w-2.5 h-2.5 rounded-full ${
                          status === 'running' ? 'bg-blue-500 animate-pulse shadow-[0_0_8px_rgba(59,130,246,0.8)]' : 
                          (status === 'success' && isOptimal === true) ? 'bg-emerald-500 shadow-[0_0_8px_rgba(16,185,129,0.8)]' : 
                          (status === 'warning' || isOptimal === false) ? 'bg-yellow-500 shadow-[0_0_8px_rgba(234,179,8,0.8)]' :
                          status === 'error' ? 'bg-red-500 shadow-[0_0_8px_rgba(239,68,68,0.8)]' :
                          'bg-text-secondary'
                        }`} />
                        <span className="text-xs font-mono uppercase opacity-70">
                          {status === 'running' ? 'Running' :
                           (status === 'success' && isOptimal === true) ? 'Improved / Optimal' :
                           (status === 'warning' || isOptimal === false) ? 'No Improvement (Timeout)' :
                           status}
                        </span>
                      </div>
                    </div>
                    <div className="flex gap-2">
                      <button 
                        onClick={() => navigator.clipboard.writeText(logs.join('\n'))}
                        disabled={logs.length === 0}
                        className="text-xs font-bold px-3 py-1 bg-border rounded hover:bg-border-input disabled:opacity-30 uppercase"
                      >
                        Copy
                      </button>
                      <button 
                        onClick={handleNext}
                        disabled={currentResults.length === 0}
                        className="text-xs font-bold px-3 py-1 bg-border rounded hover:bg-border-input disabled:opacity-30 uppercase"
                      >
                        Next Group
                      </button>
                    </div>
                  </div>
                  
                  <div
                      ref={terminalRef} 
                      className="flex-grow overflow-y-auto p-4 font-mono text-sm leading-[1.6] custom-scrollbar select-text bg-bg-accent/30"
                    >
                    {logs.map((log, i) => {
                      const isHeader = log.startsWith('#');
                      const currentNum = isHeader ? parseInt(log.substring(1, log.indexOf(':')), 10) : -1;
                      const isActive = currentNum === selectedIndex + 1;
                      const isImproved = log.startsWith('★ IMPROVED');

                      return (
                          <p key={i} className={`py-[1px] hover:bg-bg-accent/50
                          ${isImproved ? 'text-blue-600 font-bold bg-blue-500/10 px-1 rounded text-base tracking-wide' : ''}
                          ${!isImproved && log.includes('Found') ? 'text-accent-green font-bold' : ''}
                          ${isActive ? 'bg-accent-green/20 text-text-primary font-bold px-1 rounded' : (!isImproved ? 'text-text-secondary' : 'text-blue-600 font-bold')}
                          ${log.startsWith('Execution') ? 'text-blue-600' : ''}
                        `}>
                          {log}
                        </p>
                      );
                    })}
                    {logs.length === 0 && (
                      <div className="h-full flex items-center justify-center opacity-20 italic">
                        Configure parameters and click Execute...
                      </div>
                    )}
                  </div>
                </section>
              </div>
            </motion.div>
          ) : (
            /* History View */
            <motion.div 
              key="history"
              initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}
              className="grid grid-cols-12 gap-4 h-full min-h-0"
            >
              {/* Left Sidebar: File List */}
              <div className="col-span-4 flex flex-col h-full bg-bg-secondary border border-border rounded-lg min-h-0 overflow-hidden">
                 <div className="p-3 bg-bg-accent border-b border-border shrink-0">
                    <span className="text-xs font-bold tracking-widest uppercase">Saved Records</span>
                 </div>
                 <div className="flex-grow overflow-y-auto p-2 space-y-1 custom-scrollbar">
                    {historyFiles.length === 0 ? (
                      <div className="h-full flex items-center justify-center text-xs uppercase font-bold text-text-secondary opacity-30">No history found</div>
                    ) : (
                      historyFiles.map((filename) => (
                        <button
                          key={filename}
                          onClick={async () => {
                            const resp = await fetch(`http://127.0.0.1:8000/api/results/${encodeURIComponent(filename)}`);
                            const data = await resp.json();
                            setSelectedHistoryFile(filename);
                            setSelectedHistoryData(data);
                            setHistorySelectedIndex(-1);
                          }}
                          className={`w-full p-2 rounded text-left transition-all border font-mono text-sm truncate ${
                            selectedHistoryFile === filename ? 'bg-bg-accent border-border-input text-text-primary shadow-sm' : 'bg-bg-primary border-transparent text-text-secondary hover:bg-bg-accent/50'
                          }`}
                        >
                          {filename}
                        </button>
                      ))
                    )}
                 </div>
              </div>

              {/* Right Viewer: Content */}
              <div className="col-span-8 flex flex-col h-full min-h-0">
                 <section className="card h-full flex flex-col min-h-0">
                    <div className="p-3 bg-bg-accent border-b border-border flex items-center justify-between shrink-0">
                       <span className="text-xs font-bold tracking-widest uppercase">Content Viewer</span>
                       <div className="flex gap-2">
                          <button 
                            onClick={() => setHistorySelectedIndex(p => (p + 1) % (selectedHistoryData?.combinations.length || 1))} 
                            className="text-xs font-bold px-2 py-1 bg-border rounded hover:bg-border-input uppercase"
                          >
                            Next
                          </button>
                          <button onClick={deleteSelectedHistory} className="text-xs font-bold px-2 py-1 bg-red-500/10 text-red-400 rounded hover:bg-red-500/20 uppercase">Delete</button>
                       </div>
                    </div>
                    <div ref={historyRef} className="flex-grow overflow-y-auto p-6 font-mono text-sm custom-scrollbar bg-bg-accent/30">
                      {selectedHistoryData ? (
                        <div className="space-y-1">
                          <div className="bg-bg-secondary p-3 rounded border border-border mb-4">
                            <p className="text-blue-400 font-bold mb-1">Target Params:</p>
                            <p className="text-sm text-text-secondary">{JSON.stringify(selectedHistoryData.params, null, 2)}</p>
                          </div>
                          <p className="text-emerald-500 font-bold">Selected Sample Numbers:</p>
                          <p className="text-text-secondary mb-4">[{selectedHistoryData.selected_numbers.join(', ')}]</p>
                          <div className="h-px bg-border my-4" />
                          {selectedHistoryData.combinations.map((combo, idx) => (
                            <p key={idx} className={`py-0.5 hover:bg-bg-accent/50 ${historySelectedIndex === idx ? 'bg-accent-green/20 text-text-primary font-bold px-2 rounded' : 'text-text-secondary opacity-80'}`}>
                              #{(idx + 1).toString().padStart(3, '0')}: [{combo.join(', ')}]
                            </p>
                          ))}
                        </div>
                      ) : (
                        <div className="h-full flex items-center justify-center opacity-20 italic">Select a record from the left to view details...</div>
                      )}
                    </div>
                 </section>
              </div>
            </motion.div>
          )}
        </AnimatePresence>
      </main>
    </div>
  );
}
