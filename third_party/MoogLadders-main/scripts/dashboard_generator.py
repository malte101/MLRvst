#!/usr/bin/env python
"""
Dashboard Generator for MoogLadders Filter Verification.

Generates an interactive HTML dashboard from filter verification results.
Can be used standalone or imported by filter_verification.py.

Usage:
    python dashboard_generator.py filter_validation/2026-01-24_194311
    python dashboard_generator.py filter_validation/2026-01-24_194311 -o custom_dashboard.html
"""

import argparse
import json
import sys
from pathlib import Path
from typing import Optional

import numpy as np

# Lazy imports for optional dependencies
_scipy_wavfile = None


def _get_wavfile():
    """Lazy import for scipy.io.wavfile."""
    global _scipy_wavfile
    if _scipy_wavfile is None:
        from scipy.io import wavfile as wf
        _scipy_wavfile = wf
    return _scipy_wavfile


# =============================================================================
# HTML Template
# =============================================================================

DASHBOARD_TEMPLATE = '''<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>MoogLadders Filter Verification Dashboard</title>
    <script src="https://cdn.plot.ly/plotly-2.27.0.min.js"></script>
    <style>
        :root {
            --bg-primary: #1a1a2e;
            --bg-secondary: #16213e;
            --bg-card: #0f3460;
            --text-primary: #eaeaea;
            --text-secondary: #a0a0a0;
            --accent: #e94560;
            --accent-secondary: #533483;
            --success: #4ecca3;
            --warning: #ffc107;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
            background: var(--bg-primary);
            color: var(--text-primary);
            line-height: 1.6;
        }
        .container { max-width: 1600px; margin: 0 auto; padding: 20px; }
        header {
            background: linear-gradient(135deg, var(--bg-secondary), var(--accent-secondary));
            padding: 30px;
            margin-bottom: 30px;
            border-radius: 12px;
        }
        h1 { font-size: 2.5em; margin-bottom: 10px; }
        .subtitle { color: var(--text-secondary); font-size: 1.1em; }
        .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 20px; margin-bottom: 30px; }
        .card {
            background: var(--bg-card);
            padding: 20px;
            border-radius: 10px;
            text-align: center;
        }
        .card-value { font-size: 2em; font-weight: bold; color: var(--accent); }
        .card-label { color: var(--text-secondary); font-size: 0.9em; }
        .section {
            background: var(--bg-secondary);
            border-radius: 12px;
            padding: 25px;
            margin-bottom: 25px;
        }
        .section h2 {
            color: var(--accent);
            margin-bottom: 20px;
            padding-bottom: 10px;
            border-bottom: 2px solid var(--bg-card);
        }
        .controls {
            display: flex;
            flex-wrap: wrap;
            gap: 15px;
            margin-bottom: 20px;
            align-items: center;
        }
        .control-group { display: flex; flex-direction: column; gap: 5px; }
        .control-group label { font-size: 0.85em; color: var(--text-secondary); }
        select, button {
            padding: 10px 15px;
            border-radius: 6px;
            border: 1px solid var(--bg-card);
            background: var(--bg-primary);
            color: var(--text-primary);
            font-size: 1em;
            cursor: pointer;
        }
        select:focus, button:focus { outline: 2px solid var(--accent); }
        button { background: var(--accent); border: none; font-weight: bold; }
        button:hover { opacity: 0.9; }
        .plot-container { width: 100%; min-height: 450px; }
        table {
            width: 100%;
            border-collapse: collapse;
            margin-top: 15px;
        }
        th, td {
            padding: 12px;
            text-align: left;
            border-bottom: 1px solid var(--bg-card);
        }
        th { background: var(--bg-card); color: var(--accent); }
        tr:hover { background: rgba(233, 69, 96, 0.1); }
        .badge {
            display: inline-block;
            padding: 4px 10px;
            border-radius: 20px;
            font-size: 0.85em;
            font-weight: bold;
        }
        .badge-success { background: var(--success); color: #000; }
        .badge-warning { background: var(--warning); color: #000; }
        .badge-error { background: var(--accent); color: #fff; }
        .tabs { display: flex; gap: 5px; margin-bottom: 20px; flex-wrap: wrap; }
        .tab {
            padding: 10px 20px;
            background: var(--bg-card);
            border: none;
            color: var(--text-primary);
            cursor: pointer;
            border-radius: 6px 6px 0 0;
        }
        .tab.active { background: var(--accent); }
        .tab-content { display: none; }
        .tab-content.active { display: block; }
        .grid-2 { display: grid; grid-template-columns: repeat(auto-fit, minmax(700px, 1fr)); gap: 20px; }
        @media (max-width: 768px) {
            .grid-2 { grid-template-columns: 1fr; }
            h1 { font-size: 1.8em; }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>MoogLadders Filter Verification</h1>
            <p class="subtitle">Run ID: {{RUN_ID}} | {{TIMESTAMP}} | Sample Rate: {{SAMPLE_RATE}} Hz</p>
        </header>

        <div class="cards">
            <div class="card">
                <div class="card-value">{{NUM_FILTERS}}</div>
                <div class="card-label">Filters Tested</div>
            </div>
            <div class="card">
                <div class="card-value">{{NUM_TESTS}}</div>
                <div class="card-label">Test Types</div>
            </div>
            <div class="card">
                <div class="card-value">{{TOTAL_CASES}}</div>
                <div class="card-label">Total Test Cases</div>
            </div>
            <div class="card">
                <div class="card-value" id="lowestThd">-</div>
                <div class="card-label">Lowest THD</div>
            </div>
        </div>

        <div class="tabs">
            <button class="tab active" onclick="showTab('frequency')">Frequency Response</button>
            <button class="tab" onclick="showTab('thd')">THD Analysis</button>
            <button class="tab" onclick="showTab('step')">Step Response</button>
            <button class="tab" onclick="showTab('selfoscillation')">Self-Oscillation</button>
            <button class="tab" onclick="showTab('summary')">Summary Table</button>
        </div>

        <div id="frequency" class="tab-content active">
            <div class="section">
                <h2>Frequency Response Comparison</h2>
                <div class="controls">
                    <div class="control-group">
                        <label>Cutoff Frequency</label>
                        <select id="cutoffSelect" onchange="updateFreqPlot()">
                            {{CUTOFF_OPTIONS}}
                        </select>
                    </div>
                    <div class="control-group">
                        <label>Filters</label>
                        <select id="filterSelect" multiple size="4" onchange="updateFreqPlot()">
                            {{FILTER_OPTIONS}}
                        </select>
                    </div>
                </div>
                <div id="freqPlot" class="plot-container"></div>
            </div>
        </div>

        <div id="thd" class="tab-content">
            <div class="section">
                <h2>Total Harmonic Distortion</h2>
                <div id="thdPlot" class="plot-container"></div>
                <table id="thdTable">
                    <thead>
                        <tr>
                            <th>Filter</th>
                            <th>-18 dBFS</th>
                            <th>-12 dBFS</th>
                            <th>-6 dBFS</th>
                        </tr>
                    </thead>
                    <tbody id="thdTableBody"></tbody>
                </table>
            </div>
        </div>

        <div id="step" class="tab-content">
            <div class="section">
                <h2>Step Response Analysis</h2>
                <div class="controls">
                    <div class="control-group">
                        <label>Resonance (Q)</label>
                        <select id="stepResonanceSelect" onchange="updateStepPlot()">
                            <option value="0.0">Q = 0.0</option>
                            <option value="0.5">Q = 0.5</option>
                            <option value="0.9" selected>Q = 0.9</option>
                        </select>
                    </div>
                </div>
                <div id="stepPlot" class="plot-container"></div>
                <table>
                    <thead>
                        <tr>
                            <th>Filter</th>
                            <th>Overshoot (Q=0.9)</th>
                            <th>Settling Time</th>
                            <th>DC Gain</th>
                        </tr>
                    </thead>
                    <tbody id="stepTableBody"></tbody>
                </table>
            </div>
        </div>

        <div id="selfoscillation" class="tab-content">
            <div class="section">
                <h2>Self-Oscillation Test (Q=1.0)</h2>
                <div class="grid-2">
                    <div>
                        <h3 style="color: var(--success); margin-bottom: 15px;">Non-Oscillating</h3>
                        <div id="nonOscList"></div>
                    </div>
                    <div>
                        <h3 style="color: var(--accent); margin-bottom: 15px;">Self-Oscillating</h3>
                        <div id="oscList"></div>
                    </div>
                </div>
            </div>
        </div>

        <div id="summary" class="tab-content">
            <div class="section">
                <h2>Complete Results Summary</h2>
                <table>
                    <thead>
                        <tr>
                            <th>Filter</th>
                            <th>Test</th>
                            <th>Cutoff</th>
                            <th>Resonance</th>
                            <th>Key Metrics</th>
                        </tr>
                    </thead>
                    <tbody id="summaryTableBody"></tbody>
                </table>
            </div>
        </div>
    </div>

    <script>
        // Embedded data from Python
        const summaryData = {{SUMMARY_JSON}};
        const freqResponseData = {{FREQ_RESPONSE_JSON}};

        // Tab switching
        function showTab(tabId) {
            document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
            document.querySelectorAll('.tab').forEach(el => el.classList.remove('active'));
            document.getElementById(tabId).classList.add('active');
            event.target.classList.add('active');

            // Trigger plot resize
            if (tabId === 'frequency') updateFreqPlot();
            if (tabId === 'thd') updateThdPlot();
            if (tabId === 'step') updateStepPlot();
        }

        // Frequency response plot
        function updateFreqPlot() {
            const cutoff = document.getElementById('cutoffSelect').value;
            const filterSelect = document.getElementById('filterSelect');
            const selectedFilters = Array.from(filterSelect.selectedOptions).map(o => o.value);

            const traces = [];
            const colors = ['#e94560', '#4ecca3', '#ffc107', '#533483', '#00d9ff',
                           '#ff6b6b', '#95e1d3', '#f38181', '#aa96da', '#fcbad3'];

            let colorIdx = 0;
            for (const [filterName, cutoffData] of Object.entries(freqResponseData)) {
                if (selectedFilters.length > 0 && !selectedFilters.includes(filterName)) continue;
                if (!cutoffData[cutoff]) continue;

                const data = cutoffData[cutoff];
                traces.push({
                    x: data.freqs,
                    y: data.magnitude,
                    type: 'scatter',
                    mode: 'lines',
                    name: filterName,
                    line: { color: colors[colorIdx % colors.length], width: 2 }
                });
                colorIdx++;
            }

            const layout = {
                title: `Frequency Response (fc = ${cutoff} Hz)`,
                xaxis: { title: 'Frequency (Hz)', type: 'log', range: [Math.log10(20), Math.log10(20000)],
                         gridcolor: '#333', color: '#eaeaea' },
                yaxis: { title: 'Magnitude (dB)', range: [-80, 10], gridcolor: '#333', color: '#eaeaea' },
                paper_bgcolor: '#16213e',
                plot_bgcolor: '#1a1a2e',
                font: { color: '#eaeaea' },
                legend: { orientation: 'h', y: -0.2 },
                shapes: [{
                    type: 'line', x0: 20, x1: 20000, y0: -3, y1: -3,
                    line: { color: '#e94560', width: 1, dash: 'dash' }
                }]
            };

            Plotly.newPlot('freqPlot', traces, layout, {responsive: true});
        }

        // THD plot and table
        function updateThdPlot() {
            const thdResults = summaryData.results.filter(r => r.test_case === 'thd');
            const filterGroups = {};

            thdResults.forEach(r => {
                if (!filterGroups[r.filter_name]) filterGroups[r.filter_name] = {};
                filterGroups[r.filter_name][r.metrics.input_level_dbfs] = r.metrics.thd_percent;
            });

            const levels = [-18, -12, -6];
            const traces = [];
            const colors = ['#e94560', '#4ecca3', '#ffc107', '#533483', '#00d9ff',
                           '#ff6b6b', '#95e1d3', '#f38181', '#aa96da', '#fcbad3'];

            let colorIdx = 0;
            for (const [filterName, data] of Object.entries(filterGroups)) {
                traces.push({
                    x: levels,
                    y: levels.map(l => data[l] || 0),
                    type: 'scatter',
                    mode: 'lines+markers',
                    name: filterName,
                    line: { color: colors[colorIdx % colors.length], width: 2 }
                });
                colorIdx++;
            }

            const layout = {
                title: 'THD vs Input Level',
                xaxis: { title: 'Input Level (dBFS)', gridcolor: '#333', color: '#eaeaea' },
                yaxis: { title: 'THD (%)', type: 'log', gridcolor: '#333', color: '#eaeaea' },
                paper_bgcolor: '#16213e',
                plot_bgcolor: '#1a1a2e',
                font: { color: '#eaeaea' },
                legend: { orientation: 'h', y: -0.2 }
            };

            Plotly.newPlot('thdPlot', traces, layout, {responsive: true});

            // Update table
            const tbody = document.getElementById('thdTableBody');
            tbody.innerHTML = '';
            const sortedFilters = Object.entries(filterGroups)
                .sort((a, b) => (a[1][-6] || 999) - (b[1][-6] || 999));

            sortedFilters.forEach(([name, data]) => {
                const row = document.createElement('tr');
                row.innerHTML = `
                    <td><strong>${name}</strong></td>
                    <td>${(data[-18] || 0).toFixed(4)}%</td>
                    <td>${(data[-12] || 0).toFixed(4)}%</td>
                    <td>${(data[-6] || 0).toFixed(4)}%</td>
                `;
                tbody.appendChild(row);
            });

            // Update lowest THD card
            const lowestThd = Math.min(...Object.values(filterGroups).map(d => d[-18] || 999));
            document.getElementById('lowestThd').textContent = lowestThd.toFixed(4) + '%';
        }

        // Step response plot
        function updateStepPlot() {
            const resonance = parseFloat(document.getElementById('stepResonanceSelect').value);
            const stepResults = summaryData.results.filter(
                r => r.test_case === 'step' && Math.abs(r.resonance - resonance) < 0.01
            );

            const traces = [];
            const colors = ['#e94560', '#4ecca3', '#ffc107', '#533483', '#00d9ff',
                           '#ff6b6b', '#95e1d3', '#f38181', '#aa96da', '#fcbad3'];

            stepResults.forEach((r, idx) => {
                traces.push({
                    x: [r.filter_name],
                    y: [r.metrics.overshoot_pct],
                    type: 'bar',
                    name: r.filter_name,
                    marker: { color: colors[idx % colors.length] }
                });
            });

            const layout = {
                title: `Step Response Overshoot (Q = ${resonance})`,
                xaxis: { title: 'Filter', gridcolor: '#333', color: '#eaeaea' },
                yaxis: { title: 'Overshoot (%)', gridcolor: '#333', color: '#eaeaea' },
                paper_bgcolor: '#16213e',
                plot_bgcolor: '#1a1a2e',
                font: { color: '#eaeaea' },
                showlegend: false,
                barmode: 'group'
            };

            Plotly.newPlot('stepPlot', traces, layout, {responsive: true});

            // Update table
            const tbody = document.getElementById('stepTableBody');
            tbody.innerHTML = '';
            const stepAt09 = summaryData.results.filter(
                r => r.test_case === 'step' && Math.abs(r.resonance - 0.9) < 0.01
            );

            stepAt09.sort((a, b) => a.metrics.overshoot_pct - b.metrics.overshoot_pct)
                .forEach(r => {
                    const row = document.createElement('tr');
                    row.innerHTML = `
                        <td><strong>${r.filter_name}</strong></td>
                        <td>${r.metrics.overshoot_pct.toFixed(1)}%</td>
                        <td>${r.metrics.settling_time_ms.toFixed(2)} ms</td>
                        <td>${r.metrics.dc_gain.toFixed(4)}</td>
                    `;
                    tbody.appendChild(row);
                });
        }

        // Self-oscillation display
        function updateSelfOscillation() {
            const oscResults = summaryData.results.filter(r => r.test_case === 'selfoscillation');
            const oscillating = new Set();
            const nonOscillating = new Set();

            oscResults.forEach(r => {
                if (r.metrics.oscillating) {
                    oscillating.add(r.filter_name);
                } else {
                    nonOscillating.add(r.filter_name);
                }
            });

            document.getElementById('nonOscList').innerHTML = Array.from(nonOscillating)
                .map(f => `<span class="badge badge-success" style="margin: 5px;">${f}</span>`).join('');
            document.getElementById('oscList').innerHTML = Array.from(oscillating)
                .map(f => `<span class="badge badge-error" style="margin: 5px;">${f}</span>`).join('') || '<em>None</em>';
        }

        // Summary table
        function updateSummaryTable() {
            const tbody = document.getElementById('summaryTableBody');
            tbody.innerHTML = '';

            summaryData.results.slice(0, 100).forEach(r => {
                const metrics = Object.entries(r.metrics)
                    .filter(([k, v]) => typeof v === 'number')
                    .map(([k, v]) => `${k}: ${v.toFixed(4)}`)
                    .join(', ');

                const row = document.createElement('tr');
                row.innerHTML = `
                    <td><strong>${r.filter_name}</strong></td>
                    <td>${r.test_case}</td>
                    <td>${r.cutoff_hz} Hz</td>
                    <td>${r.resonance}</td>
                    <td style="font-size: 0.85em;">${metrics.substring(0, 80)}${metrics.length > 80 ? '...' : ''}</td>
                `;
                tbody.appendChild(row);
            });
        }

        // Initialize
        document.addEventListener('DOMContentLoaded', () => {
            updateFreqPlot();
            updateThdPlot();
            updateStepPlot();
            updateSelfOscillation();
            updateSummaryTable();
        });
    </script>
</body>
</html>
'''


# =============================================================================
# Helper Functions
# =============================================================================

def linear_to_dbfs(x: np.ndarray) -> np.ndarray:
    """Convert linear amplitude to dBFS."""
    return 20 * np.log10(np.abs(x) + 1e-12)


def next_power_of_2(n: int) -> int:
    """Return the next power of 2 >= n."""
    return 1 << (n - 1).bit_length()


def read_wav(path: Path) -> tuple:
    """Read a WAV file and return normalized float samples.

    Args:
        path: Input file path

    Returns:
        Tuple of (samples as float32 in [-1, 1], sample_rate)
    """
    wavfile = _get_wavfile()
    sample_rate, data = wavfile.read(str(path))

    if data.dtype == np.int16:
        samples = data.astype(np.float32) / 32767.0
    elif data.dtype == np.int32:
        samples = data.astype(np.float32) / 2147483647.0
    elif data.dtype == np.float32:
        samples = data
    else:
        samples = data.astype(np.float32)

    return samples, sample_rate


def compute_frequency_response(samples: np.ndarray, sample_rate: int) -> tuple:
    """Compute frequency response from impulse response.

    Args:
        samples: Impulse response samples
        sample_rate: Sample rate in Hz

    Returns:
        Tuple of (frequencies_hz, magnitude_db, phase_rad)
    """
    n_fft = next_power_of_2(len(samples) * 2)
    win = np.hanning(len(samples))
    windowed = samples * win

    spectrum = np.fft.rfft(windowed, n=n_fft)
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sample_rate)

    magnitude_db = linear_to_dbfs(np.abs(spectrum))
    phase_rad = np.unwrap(np.angle(spectrum))

    return freqs, magnitude_db, phase_rad


def extract_filter_name(filename: str) -> str:
    """Extract filter name from output filename.

    Args:
        filename: Filename like 'Stilson_c1000_r0.50.wav'

    Returns:
        Filter name like 'Stilson'
    """
    return filename.split('_c')[0]


# =============================================================================
# Dashboard Generation
# =============================================================================

def generate_dashboard(
    run_dir: Path,
    output_path: Optional[Path] = None,
    verbose: bool = False
) -> Path:
    """Generate an interactive HTML dashboard from test results.

    Args:
        run_dir: Path to the test run directory containing metrics/
        output_path: Optional custom output path for the HTML file
        verbose: Print progress messages

    Returns:
        Path to the generated HTML file
    """
    metrics_dir = run_dir / "metrics"
    summary_path = metrics_dir / "summary.json"

    if not summary_path.exists():
        raise FileNotFoundError(f"Summary file not found: {summary_path}")

    if verbose:
        print(f"Loading summary from {summary_path}")

    with open(summary_path, 'r') as f:
        summary = json.load(f)

    # Generate frequency response data from WAV files
    freq_response_data = {}
    wav_dir = run_dir / "wav" / "linear_response"

    if wav_dir.exists():
        if verbose:
            print("Computing frequency response data for dashboard...")

        for subdir in wav_dir.iterdir():
            if not subdir.is_dir():
                continue

            # Parse cutoff from directory name (e.g., 'c1000_r0.00_os0')
            parts = subdir.name.split('_')
            cutoff = parts[0][1:]  # e.g., 'c1000' -> '1000'

            for wav_path in subdir.glob("*.wav"):
                filter_name = extract_filter_name(wav_path.stem)

                if filter_name not in freq_response_data:
                    freq_response_data[filter_name] = {}

                try:
                    samples, sr = read_wav(wav_path)
                    freqs, mag_db, _ = compute_frequency_response(samples, sr)

                    # Downsample for smaller JSON (every 4th point)
                    freq_response_data[filter_name][cutoff] = {
                        "freqs": freqs[::4].tolist(),
                        "magnitude": mag_db[::4].tolist()
                    }
                except Exception as e:
                    if verbose:
                        print(f"  Warning: Could not process {wav_path}: {e}")

    # Build cutoff and filter options for the HTML
    cutoffs = set()
    filters = set()
    for result in summary.get('results', []):
        if result.get('test_case') == 'linear_response':
            cutoffs.add(int(result.get('cutoff_hz', 0)))
        filters.add(result.get('filter_name', ''))

    cutoff_options = '\n'.join(
        f'<option value="{c}">{c} Hz</option>'
        for c in sorted(cutoffs) if c > 0
    )
    filter_options = '\n'.join(
        f'<option value="{f}" selected>{f}</option>'
        for f in sorted(filters) if f
    )

    # Fill in the template
    html = DASHBOARD_TEMPLATE
    html = html.replace('{{RUN_ID}}', summary.get('run_id', 'Unknown'))
    html = html.replace('{{TIMESTAMP}}', summary.get('timestamp', '')[:19])
    html = html.replace('{{SAMPLE_RATE}}', str(summary.get('sample_rate', 44100)))
    html = html.replace('{{NUM_FILTERS}}', str(len(summary.get('filters_tested', []))))
    html = html.replace('{{NUM_TESTS}}', str(len(summary.get('tests_run', []))))
    html = html.replace('{{TOTAL_CASES}}', str(summary.get('total_test_cases', 0)))
    html = html.replace('{{CUTOFF_OPTIONS}}', cutoff_options)
    html = html.replace('{{FILTER_OPTIONS}}', filter_options)
    html = html.replace('{{SUMMARY_JSON}}', json.dumps(summary))
    html = html.replace('{{FREQ_RESPONSE_JSON}}', json.dumps(freq_response_data))

    # Write output
    if output_path is None:
        output_path = run_dir / "dashboard.html"

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(html)

    if verbose:
        print(f"Dashboard generated: {output_path}")

    return output_path


# =============================================================================
# CLI
# =============================================================================

def main():
    """Command-line interface for standalone dashboard generation."""
    parser = argparse.ArgumentParser(
        description='Generate interactive HTML dashboard from filter verification results.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python dashboard_generator.py filter_validation/2026-01-24_194311
  python dashboard_generator.py filter_validation/2026-01-24_194311 -o report.html
  python dashboard_generator.py filter_validation/2026-01-24_194311 --verbose
        """
    )

    parser.add_argument(
        'run_dir',
        type=Path,
        help='Path to the test run directory (e.g., filter_validation/2026-01-24_194311)'
    )

    parser.add_argument(
        '-o', '--output',
        type=Path,
        default=None,
        help='Output path for HTML file (default: <run_dir>/dashboard.html)'
    )

    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help='Print progress messages'
    )

    args = parser.parse_args()

    if not args.run_dir.exists():
        print(f"Error: Run directory not found: {args.run_dir}")
        sys.exit(1)

    try:
        dashboard_path = generate_dashboard(
            run_dir=args.run_dir,
            output_path=args.output,
            verbose=args.verbose
        )
        print(f"Dashboard generated: {dashboard_path}")
    except FileNotFoundError as e:
        print(f"Error: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"Error generating dashboard: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
