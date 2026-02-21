#!/usr/bin/env python
"""
Filter verification for the MoogLadders repo

DSP validation suite that:
1. Generates test audio signals (impulse, step, chirp, sine, noise, etc.)
2. Runs RunFilters.exe with various parameter combinations
3. Analyzes output WAV files for frequency response, THD, IMD, etc.
4. Generates PNG plots and JSON reports

Usage:
    python filter_verification.py --runfilters build/RunFilters.exe
    python filter_verification.py --runfilters build/RunFilters.exe --tests linear,thd --verbose
    python filter_verification.py --runfilters build/RunFilters.exe --filters Stilson,Huovilainen

Output:
    Creates filter_validation/<run_id>/ directory containing:
    - inputs/: Generated test signals
    - wav/: RunFilters output WAV files
    - plots/: Analysis PNG plots
    - metrics/: JSON files
"""

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass, field, asdict
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any

import numpy as np

# Lazy imports for optional heavy dependencies
_scipy_wavfile = None
_scipy_signal = None
_matplotlib_pyplot = None


def _get_wavfile():
    """Lazy import for scipy.io.wavfile."""
    global _scipy_wavfile
    if _scipy_wavfile is None:
        from scipy.io import wavfile as wf
        _scipy_wavfile = wf
    return _scipy_wavfile


def _get_signal():
    """Lazy import for scipy.signal."""
    global _scipy_signal
    if _scipy_signal is None:
        from scipy import signal as sig
        _scipy_signal = sig
    return _scipy_signal


def _get_pyplot():
    """Lazy import for matplotlib.pyplot."""
    global _matplotlib_pyplot
    if _matplotlib_pyplot is None:
        import matplotlib.pyplot as plt
        _matplotlib_pyplot = plt
    return _matplotlib_pyplot


# =============================================================================
# Constants
# =============================================================================

DEFAULT_SAMPLE_RATE = 44100
DEFAULT_BIT_DEPTH = 16

FILTER_NAMES = [
    "Stilson",
    "Simplified",
    "Huovilainen",
    "Improved",
    "Krajeski",
    "RKSimulation",
    "Microtracker",
    "MusicDSP",
    "OberheimVariation",
    "Hyperion",
]

# Default parameter grids
DEFAULT_CUTOFFS = [50, 200, 800, 1000, 2500, 5000, 12000]  # Hz
DEFAULT_RESONANCES = [0.0, 0.05, 0.1, 0.5, 0.9, 0.95, 1.0]
DEFAULT_OVERSAMPLES = [0, 2, 4, 8]

# Test case names
TEST_NAMES = [
    "linear",
    "resonance",
    "selfoscillation",
    "thd",
    "imd",
    "aliasing",
    "step",
    "noise",
]


# =============================================================================
# Dataclasses
# =============================================================================

@dataclass
class TestSignal:
    """Represents a generated test signal."""
    name: str
    samples: np.ndarray
    sample_rate: int
    description: str
    path: Optional[Path] = None


@dataclass
class TestCase:
    """Represents a single test configuration."""
    name: str
    signal_type: str
    cutoff_hz: float
    resonance: float
    oversample: int
    description: str


@dataclass
class AnalysisResult:
    """Stores analysis results for a single filter output."""
    filter_name: str
    test_case: str
    cutoff_hz: float
    resonance: float
    oversample: int
    metrics: Dict[str, Any] = field(default_factory=dict)
    plots_generated: List[str] = field(default_factory=list)


@dataclass
class MetricsSummary:
    """Aggregate metrics summary across all tests."""
    run_id: str
    timestamp: str
    sample_rate: int
    filters_tested: List[str]
    tests_run: List[str]
    total_test_cases: int
    results: List[Dict[str, Any]] = field(default_factory=list)


# =============================================================================
# Utility Functions
# =============================================================================

def linear_to_dbfs(x: np.ndarray) -> np.ndarray:
    """Convert linear amplitude to dBFS.

    Args:
        x: Linear amplitude values

    Returns:
        Amplitude in dBFS (0 dBFS = full scale)
    """
    return 20 * np.log10(np.abs(x) + 1e-12)


def dbfs_to_linear(db: float) -> float:
    """Convert dBFS to linear amplitude.

    Args:
        db: Amplitude in dBFS

    Returns:
        Linear amplitude
    """
    return 10 ** (db / 20.0)


def next_power_of_2(n: int) -> int:
    """Return the next power of 2 >= n."""
    return 1 << (n - 1).bit_length()


# =============================================================================
# WAV I/O Functions
# =============================================================================

def write_wav(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    """Write samples to a 16-bit PCM WAV file.

    Args:
        path: Output file path
        samples: Audio samples (float, normalized to [-1, 1])
        sample_rate: Sample rate in Hz
    """
    wavfile = _get_wavfile()

    # Clip and convert to 16-bit
    samples_clipped = np.clip(samples, -1.0, 1.0)
    samples_int16 = (samples_clipped * 32767).astype(np.int16)

    path.parent.mkdir(parents=True, exist_ok=True)
    wavfile.write(str(path), sample_rate, samples_int16)


def read_wav(path: Path) -> Tuple[np.ndarray, int]:
    """Read a WAV file and return normalized float samples.

    Args:
        path: Input file path

    Returns:
        Tuple of (samples as float32 in [-1, 1], sample_rate)
    """
    wavfile = _get_wavfile()

    sample_rate, data = wavfile.read(str(path))

    # Convert to float32 normalized
    if data.dtype == np.int16:
        samples = data.astype(np.float32) / 32767.0
    elif data.dtype == np.int32:
        samples = data.astype(np.float32) / 2147483647.0
    elif data.dtype == np.float32:
        samples = data
    else:
        samples = data.astype(np.float32)

    return samples, sample_rate


# =============================================================================
# Signal Generation Functions
# =============================================================================

def generate_impulse(
    sample_rate: int = DEFAULT_SAMPLE_RATE,
    length: int = 32768,
    amplitude: float = 1.0
) -> TestSignal:
    """Generate a single-sample impulse at t=0.

    Args:
        sample_rate: Sample rate in Hz
        length: Total length in samples
        amplitude: Peak amplitude (linear)

    Returns:
        TestSignal containing the impulse
    """
    samples = np.zeros(length, dtype=np.float32)
    samples[0] = amplitude

    return TestSignal(
        name=f"impulse_{sample_rate}",
        samples=samples,
        sample_rate=sample_rate,
        description=f"Unit impulse, {length} samples at {sample_rate} Hz"
    )


def generate_step(
    sample_rate: int = DEFAULT_SAMPLE_RATE,
    length: int = 32768,
    amplitude: float = 1.0
) -> TestSignal:
    """Generate a DC step from 0 to amplitude at t=0.

    Args:
        sample_rate: Sample rate in Hz
        length: Total length in samples
        amplitude: Step amplitude (linear)

    Returns:
        TestSignal containing the step response
    """
    samples = np.ones(length, dtype=np.float32) * amplitude

    return TestSignal(
        name=f"step_{sample_rate}",
        samples=samples,
        sample_rate=sample_rate,
        description=f"Unit step, {length} samples at {sample_rate} Hz"
    )


def generate_chirp(
    sample_rate: int = DEFAULT_SAMPLE_RATE,
    duration: float = 10.0,
    f0: float = 20.0,
    f1: Optional[float] = None,
    amplitude_dbfs: float = -12.0
) -> TestSignal:
    """Generate a logarithmic chirp sweep.

    Args:
        sample_rate: Sample rate in Hz
        duration: Duration in seconds
        f0: Start frequency in Hz
        f1: End frequency in Hz (default: 0.45 * sample_rate)
        amplitude_dbfs: Peak amplitude in dBFS

    Returns:
        TestSignal containing the chirp
    """
    signal = _get_signal()

    if f1 is None:
        f1 = 0.45 * sample_rate

    num_samples = int(duration * sample_rate)
    t = np.linspace(0, duration, num_samples, dtype=np.float32)

    amplitude = dbfs_to_linear(amplitude_dbfs)
    samples = amplitude * signal.chirp(t, f0, duration, f1, method='logarithmic')
    samples = samples.astype(np.float32)

    return TestSignal(
        name=f"chirp_{int(f0)}-{int(f1)}_{sample_rate}",
        samples=samples,
        sample_rate=sample_rate,
        description=f"Log chirp {f0:.0f}-{f1:.0f} Hz, {amplitude_dbfs} dBFS, {duration}s"
    )


def generate_sine(
    sample_rate: int = DEFAULT_SAMPLE_RATE,
    frequency: float = 1000.0,
    duration: float = 1.0,
    amplitude_dbfs: float = -12.0
) -> TestSignal:
    """Generate a steady sine tone.

    Args:
        sample_rate: Sample rate in Hz
        frequency: Tone frequency in Hz
        duration: Duration in seconds
        amplitude_dbfs: Peak amplitude in dBFS

    Returns:
        TestSignal containing the sine wave
    """
    num_samples = int(duration * sample_rate)
    t = np.arange(num_samples, dtype=np.float32) / sample_rate

    amplitude = dbfs_to_linear(amplitude_dbfs)
    samples = amplitude * np.sin(2 * np.pi * frequency * t).astype(np.float32)

    return TestSignal(
        name=f"sine_{int(frequency)}hz_{sample_rate}",
        samples=samples,
        sample_rate=sample_rate,
        description=f"Sine {frequency:.0f} Hz, {amplitude_dbfs} dBFS, {duration}s"
    )


def generate_two_tone(
    sample_rate: int = DEFAULT_SAMPLE_RATE,
    f1: float = 1000.0,
    f2: float = 1200.0,
    duration: float = 1.0,
    amplitude_dbfs: float = -12.0
) -> TestSignal:
    """Generate a two-tone signal for IMD testing.

    Args:
        sample_rate: Sample rate in Hz
        f1: First tone frequency in Hz
        f2: Second tone frequency in Hz
        duration: Duration in seconds
        amplitude_dbfs: Peak amplitude of each tone in dBFS

    Returns:
        TestSignal containing the two-tone signal
    """
    num_samples = int(duration * sample_rate)
    t = np.arange(num_samples, dtype=np.float32) / sample_rate

    amplitude = dbfs_to_linear(amplitude_dbfs)
    samples = amplitude * (np.sin(2 * np.pi * f1 * t) + np.sin(2 * np.pi * f2 * t))
    samples = samples.astype(np.float32)

    return TestSignal(
        name=f"twotone_{int(f1)}_{int(f2)}hz_{sample_rate}",
        samples=samples,
        sample_rate=sample_rate,
        description=f"Two-tone {f1:.0f}+{f2:.0f} Hz, {amplitude_dbfs} dBFS each"
    )


def generate_white_noise(
    sample_rate: int = DEFAULT_SAMPLE_RATE,
    duration: float = 10.0,
    rms_dbfs: float = -18.0,
    seed: int = 42
) -> TestSignal:
    """Generate white noise.

    Args:
        sample_rate: Sample rate in Hz
        duration: Duration in seconds
        rms_dbfs: RMS level in dBFS
        seed: Random seed for reproducibility

    Returns:
        TestSignal containing white noise
    """
    np.random.seed(seed)
    num_samples = int(duration * sample_rate)

    # Generate noise with unit RMS
    samples = np.random.randn(num_samples).astype(np.float32)

    # Scale to desired RMS level
    rms_linear = dbfs_to_linear(rms_dbfs)
    samples *= rms_linear

    return TestSignal(
        name=f"whitenoise_{sample_rate}",
        samples=samples,
        sample_rate=sample_rate,
        description=f"White noise, {rms_dbfs} dBFS RMS, {duration}s"
    )


def generate_near_dc_sine(
    sample_rate: int = DEFAULT_SAMPLE_RATE,
    frequency: float = 2.0,
    duration: float = 10.0,
    amplitude_dbfs: float = -6.0
) -> TestSignal:
    """Generate a near-DC sine wave for DC response testing.

    Args:
        sample_rate: Sample rate in Hz
        frequency: Low frequency in Hz
        duration: Duration in seconds
        amplitude_dbfs: Peak amplitude in dBFS

    Returns:
        TestSignal containing the low-frequency sine
    """
    num_samples = int(duration * sample_rate)
    t = np.arange(num_samples, dtype=np.float32) / sample_rate

    amplitude = dbfs_to_linear(amplitude_dbfs)
    samples = amplitude * np.sin(2 * np.pi * frequency * t).astype(np.float32)

    return TestSignal(
        name=f"neardc_{int(frequency)}hz_{sample_rate}",
        samples=samples,
        sample_rate=sample_rate,
        description=f"Near-DC sine {frequency:.1f} Hz, {amplitude_dbfs} dBFS"
    )


def generate_silence(
    sample_rate: int = DEFAULT_SAMPLE_RATE,
    length: int = 32768
) -> TestSignal:
    """Generate digital silence (all zeros).

    Args:
        sample_rate: Sample rate in Hz
        length: Total length in samples

    Returns:
        TestSignal containing silence
    """
    samples = np.zeros(length, dtype=np.float32)

    return TestSignal(
        name=f"silence_{sample_rate}",
        samples=samples,
        sample_rate=sample_rate,
        description=f"Digital silence, {length} samples"
    )


def save_test_signal(signal: TestSignal, output_dir: Path) -> Path:
    """Save a test signal to disk if not already cached.

    Args:
        signal: TestSignal to save
        output_dir: Directory for output files

    Returns:
        Path to the saved WAV file
    """
    output_path = output_dir / f"{signal.name}.wav"

    if not output_path.exists():
        write_wav(output_path, signal.samples, signal.sample_rate)

    signal.path = output_path
    return output_path


# =============================================================================
# Analysis Functions
# =============================================================================

def compute_frequency_response(
    samples: np.ndarray,
    sample_rate: int,
    window: str = "hann"
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Compute frequency response from impulse response.

    Args:
        samples: Impulse response samples
        sample_rate: Sample rate in Hz
        window: Window function name

    Returns:
        Tuple of (frequencies_hz, magnitude_db, phase_rad)
    """
    signal = _get_signal()

    # Zero-pad to next power of 2 for FFT efficiency
    n_fft = next_power_of_2(len(samples) * 2)

    # Apply window
    if window == "hann":
        win = np.hanning(len(samples))
    elif window == "blackman":
        win = np.blackman(len(samples))
    else:
        win = np.ones(len(samples))

    windowed = samples * win

    # Compute FFT
    spectrum = np.fft.rfft(windowed, n=n_fft)
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sample_rate)

    # Magnitude and phase
    magnitude_db = linear_to_dbfs(np.abs(spectrum))
    phase_rad = np.unwrap(np.angle(spectrum))

    return freqs, magnitude_db, phase_rad


def compute_group_delay(
    phase_rad: np.ndarray,
    freqs: np.ndarray
) -> np.ndarray:
    """Compute group delay from unwrapped phase.

    Args:
        phase_rad: Unwrapped phase in radians
        freqs: Frequencies in Hz

    Returns:
        Group delay in samples
    """
    # Group delay = -d(phase)/d(omega)
    omega = 2 * np.pi * freqs

    # Numerical derivative with smoothing
    d_phase = np.gradient(phase_rad)
    d_omega = np.gradient(omega)

    # Avoid division by zero
    d_omega = np.where(np.abs(d_omega) < 1e-10, 1e-10, d_omega)

    group_delay = -d_phase / d_omega

    return group_delay


def compute_thd(
    samples: np.ndarray,
    sample_rate: int,
    fundamental_freq: float,
    num_harmonics: int = 10
) -> Tuple[float, List[float]]:
    """Compute Total Harmonic Distortion.

    Args:
        samples: Audio samples
        sample_rate: Sample rate in Hz
        fundamental_freq: Expected fundamental frequency in Hz
        num_harmonics: Number of harmonics to include

    Returns:
        Tuple of (thd_percent, harmonics_db list)
    """
    n_fft = next_power_of_2(len(samples))

    # Window and FFT
    win = np.hanning(len(samples))
    spectrum = np.abs(np.fft.rfft(samples * win, n=n_fft))
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sample_rate)

    # Find bin for fundamental
    fundamental_bin = int(round(fundamental_freq * n_fft / sample_rate))

    # Get peak around fundamental (allow ±2 bins for drift)
    search_range = slice(max(0, fundamental_bin - 2), fundamental_bin + 3)
    fundamental_power = np.max(spectrum[search_range]) ** 2

    # Sum harmonic powers
    harmonic_power = 0.0
    harmonics_db = []

    for k in range(2, num_harmonics + 1):
        harmonic_bin = int(round(fundamental_freq * k * n_fft / sample_rate))
        if harmonic_bin >= len(spectrum):
            break

        search_range = slice(max(0, harmonic_bin - 2), harmonic_bin + 3)
        h_power = np.max(spectrum[search_range]) ** 2
        harmonic_power += h_power
        harmonics_db.append(linear_to_dbfs(np.sqrt(h_power)))

    # THD as percentage
    if fundamental_power > 0:
        thd_percent = np.sqrt(harmonic_power / fundamental_power) * 100
    else:
        thd_percent = 0.0

    return thd_percent, harmonics_db


def compute_imd(
    samples: np.ndarray,
    sample_rate: int,
    f1: float,
    f2: float
) -> Tuple[float, Dict[str, float]]:
    """Compute Intermodulation Distortion.

    Args:
        samples: Audio samples
        sample_rate: Sample rate in Hz
        f1: First tone frequency
        f2: Second tone frequency

    Returns:
        Tuple of (imd_db, sideband_dict)
    """
    n_fft = next_power_of_2(len(samples))

    win = np.hanning(len(samples))
    spectrum = np.abs(np.fft.rfft(samples * win, n=n_fft))
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sample_rate)

    def get_peak_power(freq: float) -> float:
        bin_idx = int(round(freq * n_fft / sample_rate))
        if bin_idx >= len(spectrum):
            return 0.0
        search_range = slice(max(0, bin_idx - 2), min(len(spectrum), bin_idx + 3))
        return np.max(spectrum[search_range]) ** 2

    # Fundamental powers
    p1 = get_peak_power(f1)
    p2 = get_peak_power(f2)
    fundamental_power = p1 + p2

    # IMD products (2nd and 3rd order)
    imd_products = {
        "f2-f1": abs(f2 - f1),
        "f1+f2": f1 + f2,
        "2f1-f2": abs(2 * f1 - f2),
        "2f2-f1": abs(2 * f2 - f1),
        "2f1+f2": 2 * f1 + f2,
        "2f2+f1": 2 * f2 + f1,
    }

    sideband_db = {}
    imd_power = 0.0

    for name, freq in imd_products.items():
        if freq < sample_rate / 2:
            power = get_peak_power(freq)
            imd_power += power
            sideband_db[name] = linear_to_dbfs(np.sqrt(power))

    # IMD as dB relative to fundamentals
    if fundamental_power > 0:
        imd_db = linear_to_dbfs(np.sqrt(imd_power / fundamental_power))
    else:
        imd_db = -120.0

    return imd_db, sideband_db


def compute_spectrogram(
    samples: np.ndarray,
    sample_rate: int,
    nperseg: int = 2048
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Compute spectrogram (time-frequency representation).

    Args:
        samples: Audio samples
        sample_rate: Sample rate in Hz
        nperseg: FFT segment length

    Returns:
        Tuple of (times, frequencies, magnitude_db)
    """
    signal = _get_signal()

    freqs, times, Sxx = signal.spectrogram(
        samples,
        fs=sample_rate,
        nperseg=nperseg,
        noverlap=nperseg // 2,
        scaling='spectrum'
    )

    magnitude_db = 10 * np.log10(Sxx + 1e-12)

    return times, freqs, magnitude_db


def compute_step_metrics(
    samples: np.ndarray,
    sample_rate: int
) -> Dict[str, float]:
    """Compute step response metrics.

    Args:
        samples: Step response samples
        sample_rate: Sample rate in Hz

    Returns:
        Dict with overshoot_pct, settling_time_ms, dc_gain
    """
    # Find steady-state value (last 10% of signal)
    steady_start = int(len(samples) * 0.9)
    dc_gain = np.mean(samples[steady_start:])

    if abs(dc_gain) < 1e-10:
        return {
            "overshoot_pct": 0.0,
            "settling_time_ms": 0.0,
            "dc_gain": 0.0,
        }

    # Overshoot
    peak = np.max(samples)
    if dc_gain > 0:
        overshoot_pct = max(0, (peak - dc_gain) / dc_gain * 100)
    else:
        overshoot_pct = 0.0

    # Settling time (2% band)
    tolerance = abs(dc_gain) * 0.02
    settled = np.abs(samples - dc_gain) < tolerance

    # Find first sample where it stays settled
    settling_sample = len(samples)
    for i in range(len(samples) - 1, -1, -1):
        if not settled[i]:
            settling_sample = i + 1
            break

    settling_time_ms = settling_sample / sample_rate * 1000

    return {
        "overshoot_pct": float(overshoot_pct),
        "settling_time_ms": float(settling_time_ms),
        "dc_gain": float(dc_gain),
    }


def compute_rms(samples: np.ndarray) -> Tuple[float, float]:
    """Compute RMS level.

    Args:
        samples: Audio samples

    Returns:
        Tuple of (rms_linear, rms_dbfs)
    """
    rms_linear = np.sqrt(np.mean(samples ** 2))
    rms_dbfs = linear_to_dbfs(rms_linear)

    return float(rms_linear), float(rms_dbfs)


def detect_self_oscillation(
    samples: np.ndarray,
    sample_rate: int,
    threshold_dbfs: float = -60.0
) -> Dict[str, Any]:
    """Detect self-oscillation in filter output.

    Args:
        samples: Filter output samples (should be from silence input)
        sample_rate: Sample rate in Hz
        threshold_dbfs: RMS threshold for detecting oscillation

    Returns:
        Dict with oscillating, dominant_freq_hz, rms_dbfs
    """
    rms_linear, rms_dbfs = compute_rms(samples)

    # Check if output has significant energy
    oscillating = rms_dbfs > threshold_dbfs

    # Find dominant frequency
    dominant_freq = 0.0
    if oscillating:
        n_fft = next_power_of_2(len(samples))
        spectrum = np.abs(np.fft.rfft(samples, n=n_fft))
        freqs = np.fft.rfftfreq(n_fft, 1.0 / sample_rate)
        dominant_freq = freqs[np.argmax(spectrum)]

    return {
        "oscillating": oscillating,
        "dominant_freq_hz": float(dominant_freq),
        "rms_dbfs": float(rms_dbfs),
    }


def compute_psd(
    samples: np.ndarray,
    sample_rate: int,
    nperseg: int = 4096
) -> Tuple[np.ndarray, np.ndarray]:
    """Compute power spectral density.

    Args:
        samples: Audio samples
        sample_rate: Sample rate in Hz
        nperseg: Segment length for Welch's method

    Returns:
        Tuple of (frequencies, psd_db)
    """
    signal = _get_signal()

    freqs, psd = signal.welch(samples, fs=sample_rate, nperseg=nperseg)
    psd_db = 10 * np.log10(psd + 1e-12)

    return freqs, psd_db


# =============================================================================
# Plot Generation Functions
# =============================================================================

def plot_frequency_response(
    freqs: np.ndarray,
    magnitude_db: np.ndarray,
    output_path: Path,
    title: str = "Frequency Response",
    cutoff_hz: Optional[float] = None
) -> Path:
    """Plot magnitude frequency response.

    Args:
        freqs: Frequencies in Hz
        magnitude_db: Magnitude in dB
        output_path: Output PNG path
        title: Plot title
        cutoff_hz: Optional cutoff frequency to mark

    Returns:
        Path to saved PNG
    """
    plt = _get_pyplot()

    fig, ax = plt.subplots(figsize=(10, 6))

    ax.semilogx(freqs[1:], magnitude_db[1:], linewidth=1.5)

    ax.axhline(y=-3, color='r', linestyle='--', alpha=0.5, label='-3 dB')
    if cutoff_hz:
        ax.axvline(x=cutoff_hz, color='g', linestyle='--', alpha=0.5, label=f'Cutoff: {cutoff_hz} Hz')

    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_title(title)
    ax.set_xlim([20, freqs[-1]])
    ax.set_ylim([-80, 10])
    ax.grid(True, alpha=0.3, which='both')
    ax.legend()

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(output_path), dpi=150, bbox_inches='tight')
    plt.close(fig)

    return output_path


def plot_phase_response(
    freqs: np.ndarray,
    phase_rad: np.ndarray,
    output_path: Path,
    title: str = "Phase Response"
) -> Path:
    """Plot phase response.

    Args:
        freqs: Frequencies in Hz
        phase_rad: Phase in radians (unwrapped)
        output_path: Output PNG path
        title: Plot title

    Returns:
        Path to saved PNG
    """
    plt = _get_pyplot()

    fig, ax = plt.subplots(figsize=(10, 6))

    phase_deg = np.rad2deg(phase_rad)
    ax.semilogx(freqs[1:], phase_deg[1:], linewidth=1.5)

    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Phase (degrees)')
    ax.set_title(title)
    ax.set_xlim([20, freqs[-1]])
    ax.grid(True, alpha=0.3, which='both')

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(output_path), dpi=150, bbox_inches='tight')
    plt.close(fig)

    return output_path


def plot_group_delay(
    freqs: np.ndarray,
    group_delay: np.ndarray,
    output_path: Path,
    sample_rate: int,
    title: str = "Group Delay"
) -> Path:
    """Plot group delay.

    Args:
        freqs: Frequencies in Hz
        group_delay: Group delay in samples
        output_path: Output PNG path
        sample_rate: Sample rate for ms conversion
        title: Plot title

    Returns:
        Path to saved PNG
    """
    plt = _get_pyplot()

    fig, ax = plt.subplots(figsize=(10, 6))

    # Convert to milliseconds
    group_delay_ms = group_delay / sample_rate * 1000

    ax.semilogx(freqs[1:], group_delay_ms[1:], linewidth=1.5)

    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Group Delay (ms)')
    ax.set_title(title)
    ax.set_xlim([20, freqs[-1]])
    ax.grid(True, alpha=0.3, which='both')

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(output_path), dpi=150, bbox_inches='tight')
    plt.close(fig)

    return output_path


def plot_harmonic_spectrum(
    samples: np.ndarray,
    sample_rate: int,
    fundamental_freq: float,
    output_path: Path,
    title: str = "Harmonic Spectrum"
) -> Path:
    """Plot FFT with harmonic annotations.

    Args:
        samples: Audio samples
        sample_rate: Sample rate in Hz
        fundamental_freq: Fundamental frequency in Hz
        output_path: Output PNG path
        title: Plot title

    Returns:
        Path to saved PNG
    """
    plt = _get_pyplot()

    n_fft = next_power_of_2(len(samples))
    win = np.hanning(len(samples))
    spectrum = np.abs(np.fft.rfft(samples * win, n=n_fft))
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sample_rate)

    magnitude_db = linear_to_dbfs(spectrum)

    fig, ax = plt.subplots(figsize=(12, 6))

    ax.plot(freqs, magnitude_db, linewidth=0.5, alpha=0.8)

    # Mark harmonics
    for k in range(1, 11):
        h_freq = fundamental_freq * k
        if h_freq < sample_rate / 2:
            ax.axvline(x=h_freq, color='r' if k == 1 else 'orange',
                       linestyle='--', alpha=0.5, linewidth=0.5)
            ax.annotate(f'H{k}', (h_freq, ax.get_ylim()[1] - 5),
                       fontsize=8, ha='center')

    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dBFS)')
    ax.set_title(title)
    ax.set_xlim([0, min(fundamental_freq * 12, sample_rate / 2)])
    ax.set_ylim([-120, 0])
    ax.grid(True, alpha=0.3)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(output_path), dpi=150, bbox_inches='tight')
    plt.close(fig)

    return output_path


def plot_imd_spectrum(
    samples: np.ndarray,
    sample_rate: int,
    f1: float,
    f2: float,
    output_path: Path,
    title: str = "IMD Spectrum"
) -> Path:
    """Plot FFT with IMD product annotations.

    Args:
        samples: Audio samples
        sample_rate: Sample rate in Hz
        f1: First tone frequency
        f2: Second tone frequency
        output_path: Output PNG path
        title: Plot title

    Returns:
        Path to saved PNG
    """
    plt = _get_pyplot()

    n_fft = next_power_of_2(len(samples))
    win = np.hanning(len(samples))
    spectrum = np.abs(np.fft.rfft(samples * win, n=n_fft))
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sample_rate)

    magnitude_db = linear_to_dbfs(spectrum)

    fig, ax = plt.subplots(figsize=(12, 6))

    ax.plot(freqs, magnitude_db, linewidth=0.5, alpha=0.8)

    # Mark fundamentals
    ax.axvline(x=f1, color='green', linestyle='-', alpha=0.7, linewidth=1, label=f'f1={f1}Hz')
    ax.axvline(x=f2, color='blue', linestyle='-', alpha=0.7, linewidth=1, label=f'f2={f2}Hz')

    # Mark IMD products
    imd_freqs = [
        (abs(f2 - f1), 'f2-f1'),
        (2 * f1 - f2, '2f1-f2'),
        (2 * f2 - f1, '2f2-f1'),
    ]

    for freq, name in imd_freqs:
        if 0 < freq < sample_rate / 2:
            ax.axvline(x=freq, color='red', linestyle='--', alpha=0.5, linewidth=0.5)
            ax.annotate(name, (freq, ax.get_ylim()[1] - 5), fontsize=7, ha='center', color='red')

    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dBFS)')
    ax.set_title(title)
    ax.set_xlim([0, f2 * 3])
    ax.set_ylim([-120, 0])
    ax.grid(True, alpha=0.3)
    ax.legend(loc='upper right')

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(output_path), dpi=150, bbox_inches='tight')
    plt.close(fig)

    return output_path


def plot_spectrogram(
    times: np.ndarray,
    freqs: np.ndarray,
    magnitude_db: np.ndarray,
    output_path: Path,
    title: str = "Spectrogram"
) -> Path:
    """Plot time-frequency spectrogram.

    Args:
        times: Time values
        freqs: Frequency values
        magnitude_db: Magnitude in dB
        output_path: Output PNG path
        title: Plot title

    Returns:
        Path to saved PNG
    """
    plt = _get_pyplot()

    fig, ax = plt.subplots(figsize=(12, 6))

    pcm = ax.pcolormesh(times, freqs, magnitude_db, shading='gouraud',
                        vmin=-80, vmax=0, cmap='magma')

    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Frequency (Hz)')
    ax.set_title(title)
    ax.set_ylim([0, freqs[-1]])

    fig.colorbar(pcm, ax=ax, label='Magnitude (dB)')

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(output_path), dpi=150, bbox_inches='tight')
    plt.close(fig)

    return output_path


def plot_step_response(
    samples: np.ndarray,
    sample_rate: int,
    output_path: Path,
    title: str = "Step Response"
) -> Path:
    """Plot time-domain step response.

    Args:
        samples: Step response samples
        sample_rate: Sample rate in Hz
        output_path: Output PNG path
        title: Plot title

    Returns:
        Path to saved PNG
    """
    plt = _get_pyplot()

    fig, ax = plt.subplots(figsize=(10, 6))

    # Show first 50ms
    num_samples = min(len(samples), int(0.05 * sample_rate))
    time_ms = np.arange(num_samples) / sample_rate * 1000

    ax.plot(time_ms, samples[:num_samples], linewidth=1.5)

    # Mark steady state
    dc = np.mean(samples[int(len(samples) * 0.9):])
    ax.axhline(y=dc, color='g', linestyle='--', alpha=0.5, label=f'DC: {dc:.3f}')

    ax.set_xlabel('Time (ms)')
    ax.set_ylabel('Amplitude')
    ax.set_title(title)
    ax.grid(True, alpha=0.3)
    ax.legend()

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(output_path), dpi=150, bbox_inches='tight')
    plt.close(fig)

    return output_path


def plot_self_oscillation(
    samples: np.ndarray,
    sample_rate: int,
    output_path: Path,
    title: str = "Self-Oscillation Analysis"
) -> Path:
    """Plot waveform and spectrum for self-oscillation check.

    Args:
        samples: Filter output samples
        sample_rate: Sample rate in Hz
        output_path: Output PNG path
        title: Plot title

    Returns:
        Path to saved PNG
    """
    plt = _get_pyplot()

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 8))

    # Waveform (first 50ms)
    num_samples = min(len(samples), int(0.05 * sample_rate))
    time_ms = np.arange(num_samples) / sample_rate * 1000
    ax1.plot(time_ms, samples[:num_samples], linewidth=0.5)
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Amplitude')
    ax1.set_title('Waveform')
    ax1.grid(True, alpha=0.3)

    # Spectrum
    n_fft = next_power_of_2(len(samples))
    spectrum = np.abs(np.fft.rfft(samples, n=n_fft))
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sample_rate)
    magnitude_db = linear_to_dbfs(spectrum)

    ax2.plot(freqs, magnitude_db, linewidth=0.5)
    ax2.set_xlabel('Frequency (Hz)')
    ax2.set_ylabel('Magnitude (dBFS)')
    ax2.set_title('Spectrum')
    ax2.set_xlim([0, sample_rate / 2])
    ax2.set_ylim([-120, 0])
    ax2.grid(True, alpha=0.3)

    fig.suptitle(title)
    fig.tight_layout()

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(output_path), dpi=150, bbox_inches='tight')
    plt.close(fig)

    return output_path


def plot_comparison(
    results: List[Tuple[str, np.ndarray, np.ndarray]],
    output_path: Path,
    xlabel: str = "Frequency (Hz)",
    ylabel: str = "Magnitude (dB)",
    title: str = "Filter Comparison",
    log_x: bool = True
) -> Path:
    """Plot multiple filters overlaid for comparison.

    Args:
        results: List of (filter_name, x_data, y_data) tuples
        output_path: Output PNG path
        xlabel: X-axis label
        ylabel: Y-axis label
        title: Plot title
        log_x: Use logarithmic x-axis

    Returns:
        Path to saved PNG
    """
    plt = _get_pyplot()

    fig, ax = plt.subplots(figsize=(12, 8))

    for name, x, y in results:
        if log_x:
            ax.semilogx(x[1:], y[1:], linewidth=1.5, label=name)
        else:
            ax.plot(x, y, linewidth=1.5, label=name)

    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, alpha=0.3, which='both')
    ax.legend(loc='best', fontsize=8)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(output_path), dpi=150, bbox_inches='tight')
    plt.close(fig)

    return output_path


# =============================================================================
# RunFilters Integration
# =============================================================================

def run_filters(
    runfilters_path: Path,
    input_wav: Path,
    cutoff: float,
    resonance: float,
    oversample: int,
    output_dir: Path,
    verbose: bool = False
) -> List[Path]:
    """Execute RunFilters.exe and return output file paths.

    Args:
        runfilters_path: Path to RunFilters.exe
        input_wav: Input WAV file path
        cutoff: Cutoff frequency in Hz
        resonance: Resonance value (0-1)
        oversample: Oversampling factor (0 for none)
        output_dir: Directory for output files
        verbose: Print command output

    Returns:
        List of paths to generated WAV files
    """
    output_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(runfilters_path),
        '-f', str(input_wav),
        '-c', str(int(cutoff)),
        '-r', f'{resonance:.2f}',
        '-s', str(oversample),
        '-o', str(output_dir)
    ]

    if verbose:
        print(f"Running: {' '.join(cmd)}")

    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode != 0:
        print(f"RunFilters error: {result.stderr}")
        return []

    if verbose and result.stdout:
        print(result.stdout)

    # Find generated files
    if oversample > 0:
        pattern = f"*_c{int(cutoff)}_r{resonance:.2f}_os{oversample}x.wav"
    else:
        pattern = f"*_c{int(cutoff)}_r{resonance:.2f}.wav"

    output_files = list(output_dir.glob(pattern))

    return output_files


def extract_filter_name(filename: str) -> str:
    """Extract filter name from output filename.

    Args:
        filename: Filename like 'Stilson_c1000_r0.50.wav'

    Returns:
        Filter name like 'Stilson'
    """
    return filename.split('_c')[0]


# =============================================================================
# Test Orchestration
# =============================================================================

def run_linear_response_test(
    runfilters_path: Path,
    inputs_dir: Path,
    wav_dir: Path,
    plots_dir: Path,
    sample_rate: int,
    cutoffs: List[float],
    filters: List[str],
    oversamples: List[int],
    skip_existing: bool,
    no_plots: bool,
    verbose: bool
) -> List[AnalysisResult]:
    """Run linear frequency response tests.

    Args:
        runfilters_path: Path to RunFilters.exe
        inputs_dir: Directory for input signals
        wav_dir: Directory for output WAVs
        plots_dir: Directory for plots
        sample_rate: Sample rate in Hz
        cutoffs: List of cutoff frequencies to test
        filters: List of filter names to include
        oversamples: List of oversample factors
        skip_existing: Skip tests with existing outputs
        no_plots: Skip plot generation
        verbose: Print progress

    Returns:
        List of AnalysisResult objects
    """
    results = []
    resonance = 0.0

    # Generate impulse signal
    impulse = generate_impulse(sample_rate=sample_rate)
    impulse_path = save_test_signal(impulse, inputs_dir)

    for cutoff in cutoffs:
        for oversample in oversamples:
            if verbose:
                print(f"  Linear response: cutoff={cutoff}, os={oversample}x")

            test_dir = wav_dir / "linear_response" / f"c{int(cutoff)}_r{resonance:.2f}_os{oversample}"

            # Check if we should skip
            if skip_existing and test_dir.exists() and list(test_dir.glob("*.wav")):
                output_files = list(test_dir.glob("*.wav"))
            else:
                output_files = run_filters(
                    runfilters_path, impulse_path, cutoff, resonance,
                    oversample, test_dir, verbose
                )

            for wav_path in output_files:
                filter_name = extract_filter_name(wav_path.stem)

                if filter_name not in filters:
                    continue

                samples, sr = read_wav(wav_path)

                # Compute frequency response
                freqs, mag_db, phase_rad = compute_frequency_response(samples, sr)
                group_delay = compute_group_delay(phase_rad, freqs)

                result = AnalysisResult(
                    filter_name=filter_name,
                    test_case="linear_response",
                    cutoff_hz=cutoff,
                    resonance=resonance,
                    oversample=oversample,
                    metrics={
                        "cutoff_measured_hz": float(freqs[np.argmin(np.abs(mag_db - mag_db[0] + 3))]),
                        "passband_ripple_db": float(np.max(mag_db[:int(len(mag_db) * 0.1)]) - np.min(mag_db[:int(len(mag_db) * 0.1)])),
                        "stopband_atten_db": float(-np.min(mag_db[int(len(mag_db) * 0.6):])),
                    }
                )

                if not no_plots:
                    os_suffix = f"_os{oversample}x" if oversample > 0 else ""

                    mag_plot = plot_frequency_response(
                        freqs, mag_db,
                        plots_dir / "linear_response" / f"{filter_name}_c{int(cutoff)}{os_suffix}_magnitude.png",
                        f"{filter_name} Magnitude Response (fc={cutoff}Hz)",
                        cutoff
                    )
                    result.plots_generated.append(str(mag_plot))

                    phase_plot = plot_phase_response(
                        freqs, phase_rad,
                        plots_dir / "linear_response" / f"{filter_name}_c{int(cutoff)}{os_suffix}_phase.png",
                        f"{filter_name} Phase Response (fc={cutoff}Hz)"
                    )
                    result.plots_generated.append(str(phase_plot))

                results.append(result)

    return results


def run_resonance_test(
    runfilters_path: Path,
    inputs_dir: Path,
    wav_dir: Path,
    plots_dir: Path,
    sample_rate: int,
    cutoffs: List[float],
    filters: List[str],
    skip_existing: bool,
    no_plots: bool,
    verbose: bool
) -> List[AnalysisResult]:
    """Run resonance peak tracking tests."""
    results = []
    resonance = 0.9
    oversample = 0

    chirp = generate_chirp(sample_rate=sample_rate, duration=5.0)
    chirp_path = save_test_signal(chirp, inputs_dir)

    test_cutoffs = [c for c in cutoffs if c in [200, 1000, 5000]]

    for cutoff in test_cutoffs:
        if verbose:
            print(f"  Resonance test: cutoff={cutoff}")

        test_dir = wav_dir / "resonance" / f"c{int(cutoff)}_r{resonance:.2f}_os{oversample}"

        if skip_existing and test_dir.exists() and list(test_dir.glob("*.wav")):
            output_files = list(test_dir.glob("*.wav"))
        else:
            output_files = run_filters(
                runfilters_path, chirp_path, cutoff, resonance,
                oversample, test_dir, verbose
            )

        for wav_path in output_files:
            filter_name = extract_filter_name(wav_path.stem)

            if filter_name not in filters:
                continue

            samples, sr = read_wav(wav_path)
            times, freqs, spec_db = compute_spectrogram(samples, sr)

            # Find peak in spectrogram around cutoff
            freq_mask = (freqs > cutoff * 0.5) & (freqs < cutoff * 1.5)
            if np.any(freq_mask):
                peak_freq_idx = np.argmax(np.mean(spec_db[freq_mask, :], axis=1))
                peak_freq = freqs[freq_mask][peak_freq_idx]
                peak_amp = np.max(spec_db[freq_mask, :])
            else:
                peak_freq = cutoff
                peak_amp = -80

            result = AnalysisResult(
                filter_name=filter_name,
                test_case="resonance",
                cutoff_hz=cutoff,
                resonance=resonance,
                oversample=oversample,
                metrics={
                    "peak_freq_hz": float(peak_freq),
                    "peak_amplitude_db": float(peak_amp),
                    "freq_error_percent": float(abs(peak_freq - cutoff) / cutoff * 100),
                }
            )

            if not no_plots:
                spec_plot = plot_spectrogram(
                    times, freqs, spec_db,
                    plots_dir / "resonance" / f"{filter_name}_c{int(cutoff)}_spectrogram.png",
                    f"{filter_name} Resonance Test (fc={cutoff}Hz, Q=0.9)"
                )
                result.plots_generated.append(str(spec_plot))

            results.append(result)

    return results


def run_selfoscillation_test(
    runfilters_path: Path,
    inputs_dir: Path,
    wav_dir: Path,
    plots_dir: Path,
    sample_rate: int,
    cutoffs: List[float],
    filters: List[str],
    skip_existing: bool,
    no_plots: bool,
    verbose: bool
) -> List[AnalysisResult]:
    """Run self-oscillation detection tests."""
    results = []
    resonance = 1.0
    oversample = 0

    silence = generate_silence(sample_rate=sample_rate, length=sample_rate * 2)
    silence_path = save_test_signal(silence, inputs_dir)

    test_cutoffs = [c for c in cutoffs if c in [200, 1000, 5000]]

    for cutoff in test_cutoffs:
        if verbose:
            print(f"  Self-oscillation test: cutoff={cutoff}")

        test_dir = wav_dir / "selfoscillation" / f"c{int(cutoff)}_r{resonance:.2f}_os{oversample}"

        if skip_existing and test_dir.exists() and list(test_dir.glob("*.wav")):
            output_files = list(test_dir.glob("*.wav"))
        else:
            output_files = run_filters(
                runfilters_path, silence_path, cutoff, resonance,
                oversample, test_dir, verbose
            )

        for wav_path in output_files:
            filter_name = extract_filter_name(wav_path.stem)

            if filter_name not in filters:
                continue

            samples, sr = read_wav(wav_path)
            osc_info = detect_self_oscillation(samples, sr)

            result = AnalysisResult(
                filter_name=filter_name,
                test_case="selfoscillation",
                cutoff_hz=cutoff,
                resonance=resonance,
                oversample=oversample,
                metrics=osc_info
            )

            if not no_plots:
                osc_plot = plot_self_oscillation(
                    samples, sr,
                    plots_dir / "selfoscillation" / f"{filter_name}_c{int(cutoff)}_oscillation.png",
                    f"{filter_name} Self-Oscillation (fc={cutoff}Hz)"
                )
                result.plots_generated.append(str(osc_plot))

            results.append(result)

    return results


def run_thd_test(
    runfilters_path: Path,
    inputs_dir: Path,
    wav_dir: Path,
    plots_dir: Path,
    sample_rate: int,
    filters: List[str],
    skip_existing: bool,
    no_plots: bool,
    verbose: bool
) -> List[AnalysisResult]:
    """Run THD measurements at different levels."""
    results = []
    cutoff = 5000  # Set cutoff above test frequency
    resonance = 0.0
    oversample = 0
    test_freq = 1000

    for level_dbfs in [-18, -12, -6]:
        if verbose:
            print(f"  THD test: level={level_dbfs} dBFS")

        sine = generate_sine(sample_rate=sample_rate, frequency=test_freq,
                            duration=1.0, amplitude_dbfs=level_dbfs)
        sine.name = f"sine_{test_freq}hz_{level_dbfs}dbfs_{sample_rate}"
        sine_path = save_test_signal(sine, inputs_dir)

        test_dir = wav_dir / "thd" / f"level{level_dbfs}dbfs"

        if skip_existing and test_dir.exists() and list(test_dir.glob("*.wav")):
            output_files = list(test_dir.glob("*.wav"))
        else:
            output_files = run_filters(
                runfilters_path, sine_path, cutoff, resonance,
                oversample, test_dir, verbose
            )

        for wav_path in output_files:
            filter_name = extract_filter_name(wav_path.stem)

            if filter_name not in filters:
                continue

            samples, sr = read_wav(wav_path)
            thd_pct, harmonics = compute_thd(samples, sr, test_freq)

            result = AnalysisResult(
                filter_name=filter_name,
                test_case="thd",
                cutoff_hz=cutoff,
                resonance=resonance,
                oversample=oversample,
                metrics={
                    "input_level_dbfs": level_dbfs,
                    "thd_percent": float(thd_pct),
                    "harmonics_db": [float(h) for h in harmonics[:5]],
                }
            )

            if not no_plots:
                harm_plot = plot_harmonic_spectrum(
                    samples, sr, test_freq,
                    plots_dir / "thd" / f"{filter_name}_{level_dbfs}dbfs_harmonics.png",
                    f"{filter_name} THD ({level_dbfs} dBFS input)"
                )
                result.plots_generated.append(str(harm_plot))

            results.append(result)

    return results


def run_imd_test(
    runfilters_path: Path,
    inputs_dir: Path,
    wav_dir: Path,
    plots_dir: Path,
    sample_rate: int,
    filters: List[str],
    skip_existing: bool,
    no_plots: bool,
    verbose: bool
) -> List[AnalysisResult]:
    """Run IMD measurements."""
    results = []
    cutoff = 2500
    resonance = 0.0
    oversample = 0
    f1, f2 = 1000, 1200

    if verbose:
        print(f"  IMD test: {f1}+{f2} Hz")

    twotone = generate_two_tone(sample_rate=sample_rate, f1=f1, f2=f2, duration=1.0)
    twotone_path = save_test_signal(twotone, inputs_dir)

    test_dir = wav_dir / "imd"

    if skip_existing and test_dir.exists() and list(test_dir.glob("*.wav")):
        output_files = list(test_dir.glob("*.wav"))
    else:
        output_files = run_filters(
            runfilters_path, twotone_path, cutoff, resonance,
            oversample, test_dir, verbose
        )

    for wav_path in output_files:
        filter_name = extract_filter_name(wav_path.stem)

        if filter_name not in filters:
            continue

        samples, sr = read_wav(wav_path)
        imd_db, sidebands = compute_imd(samples, sr, f1, f2)

        result = AnalysisResult(
            filter_name=filter_name,
            test_case="imd",
            cutoff_hz=cutoff,
            resonance=resonance,
            oversample=oversample,
            metrics={
                "imd_db": float(imd_db),
                "sidebands": {k: float(v) for k, v in sidebands.items()},
            }
        )

        if not no_plots:
            imd_plot = plot_imd_spectrum(
                samples, sr, f1, f2,
                plots_dir / "imd" / f"{filter_name}_imd.png",
                f"{filter_name} IMD ({f1}+{f2} Hz)"
            )
            result.plots_generated.append(str(imd_plot))

        results.append(result)

    return results


def run_aliasing_test(
    runfilters_path: Path,
    inputs_dir: Path,
    wav_dir: Path,
    plots_dir: Path,
    sample_rate: int,
    filters: List[str],
    oversamples: List[int],
    skip_existing: bool,
    no_plots: bool,
    verbose: bool
) -> List[AnalysisResult]:
    """Run aliasing stress tests comparing oversampling factors."""
    results = []
    cutoff = 15000
    resonance = 0.9

    # High frequency test tones
    test_freqs = [10000, 15000]

    for test_freq in test_freqs:
        if verbose:
            print(f"  Aliasing test: {test_freq} Hz")

        sine = generate_sine(sample_rate=sample_rate, frequency=test_freq,
                            duration=1.0, amplitude_dbfs=-6)
        sine.name = f"sine_{test_freq}hz_alias_{sample_rate}"
        sine_path = save_test_signal(sine, inputs_dir)

        for oversample in [0, 4]:
            if oversample not in oversamples:
                continue

            test_dir = wav_dir / "aliasing" / f"f{test_freq}_os{oversample}"

            if skip_existing and test_dir.exists() and list(test_dir.glob("*.wav")):
                output_files = list(test_dir.glob("*.wav"))
            else:
                output_files = run_filters(
                    runfilters_path, sine_path, cutoff, resonance,
                    oversample, test_dir, verbose
                )

            for wav_path in output_files:
                filter_name = extract_filter_name(wav_path.stem)

                if filter_name not in filters:
                    continue

                samples, sr = read_wav(wav_path)

                # Compute energy in alias bands (below original frequency)
                n_fft = next_power_of_2(len(samples))
                spectrum = np.abs(np.fft.rfft(samples, n=n_fft))
                freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)

                # Energy in expected band
                main_mask = (freqs > test_freq * 0.9) & (freqs < test_freq * 1.1)
                main_energy = np.sum(spectrum[main_mask] ** 2)

                # Energy in alias band (below fundamental, excluding DC)
                alias_mask = (freqs > 100) & (freqs < test_freq * 0.5)
                alias_energy = np.sum(spectrum[alias_mask] ** 2)

                if main_energy > 0:
                    alias_ratio_db = 10 * np.log10(alias_energy / main_energy + 1e-12)
                else:
                    alias_ratio_db = -120

                result = AnalysisResult(
                    filter_name=filter_name,
                    test_case="aliasing",
                    cutoff_hz=cutoff,
                    resonance=resonance,
                    oversample=oversample,
                    metrics={
                        "test_freq_hz": test_freq,
                        "alias_ratio_db": float(alias_ratio_db),
                    }
                )

                results.append(result)

    return results


def run_step_test(
    runfilters_path: Path,
    inputs_dir: Path,
    wav_dir: Path,
    plots_dir: Path,
    sample_rate: int,
    filters: List[str],
    skip_existing: bool,
    no_plots: bool,
    verbose: bool
) -> List[AnalysisResult]:
    """Run step response tests at different resonance levels."""
    results = []
    cutoff = 1000
    oversample = 0

    step = generate_step(sample_rate=sample_rate)
    step_path = save_test_signal(step, inputs_dir)

    for resonance in [0.0, 0.5, 0.9]:
        if verbose:
            print(f"  Step response test: resonance={resonance}")

        test_dir = wav_dir / "step" / f"r{resonance:.2f}"

        if skip_existing and test_dir.exists() and list(test_dir.glob("*.wav")):
            output_files = list(test_dir.glob("*.wav"))
        else:
            output_files = run_filters(
                runfilters_path, step_path, cutoff, resonance,
                oversample, test_dir, verbose
            )

        for wav_path in output_files:
            filter_name = extract_filter_name(wav_path.stem)

            if filter_name not in filters:
                continue

            samples, sr = read_wav(wav_path)
            step_metrics = compute_step_metrics(samples, sr)

            result = AnalysisResult(
                filter_name=filter_name,
                test_case="step",
                cutoff_hz=cutoff,
                resonance=resonance,
                oversample=oversample,
                metrics=step_metrics
            )

            if not no_plots:
                step_plot = plot_step_response(
                    samples, sr,
                    plots_dir / "step" / f"{filter_name}_r{resonance:.2f}_step.png",
                    f"{filter_name} Step Response (Q={resonance})"
                )
                result.plots_generated.append(str(step_plot))

            results.append(result)

    return results


def run_noise_test(
    runfilters_path: Path,
    inputs_dir: Path,
    wav_dir: Path,
    plots_dir: Path,
    sample_rate: int,
    filters: List[str],
    skip_existing: bool,
    no_plots: bool,
    verbose: bool
) -> List[AnalysisResult]:
    """Run noise shaping / PSD analysis tests."""
    results = []
    cutoff = 1000
    oversample = 0

    noise = generate_white_noise(sample_rate=sample_rate, duration=5.0)
    noise_path = save_test_signal(noise, inputs_dir)

    # Compute input PSD for reference
    input_freqs, input_psd = compute_psd(noise.samples, sample_rate)

    for resonance in [0.0, 0.9]:
        if verbose:
            print(f"  Noise shaping test: resonance={resonance}")

        test_dir = wav_dir / "noise" / f"r{resonance:.2f}"

        if skip_existing and test_dir.exists() and list(test_dir.glob("*.wav")):
            output_files = list(test_dir.glob("*.wav"))
        else:
            output_files = run_filters(
                runfilters_path, noise_path, cutoff, resonance,
                oversample, test_dir, verbose
            )

        for wav_path in output_files:
            filter_name = extract_filter_name(wav_path.stem)

            if filter_name not in filters:
                continue

            samples, sr = read_wav(wav_path)
            output_freqs, output_psd = compute_psd(samples, sr)

            # Compute attenuation at cutoff and in stopband
            cutoff_idx = np.argmin(np.abs(output_freqs - cutoff))
            stopband_idx = np.argmin(np.abs(output_freqs - cutoff * 4))

            result = AnalysisResult(
                filter_name=filter_name,
                test_case="noise",
                cutoff_hz=cutoff,
                resonance=resonance,
                oversample=oversample,
                metrics={
                    "psd_at_cutoff_db": float(output_psd[cutoff_idx]),
                    "psd_at_4x_cutoff_db": float(output_psd[stopband_idx]),
                    "stopband_atten_db": float(input_psd[stopband_idx] - output_psd[stopband_idx]),
                }
            )

            results.append(result)

    return results


def generate_comparison_plots(
    wav_dir: Path,
    plots_dir: Path,
    sample_rate: int,
    filters: List[str],
    verbose: bool
) -> List[str]:
    """Generate comparison plots overlaying all filters."""
    plots = []

    # Find all linear response outputs for comparison
    linear_dir = wav_dir / "linear_response"
    if not linear_dir.exists():
        return plots

    # Group by cutoff
    cutoff_groups = {}
    for subdir in linear_dir.iterdir():
        if not subdir.is_dir():
            continue

        # Parse cutoff from directory name
        parts = subdir.name.split('_')
        cutoff = int(parts[0][1:])  # e.g., 'c1000' -> 1000

        if cutoff not in cutoff_groups:
            cutoff_groups[cutoff] = []

        for wav_path in subdir.glob("*.wav"):
            filter_name = extract_filter_name(wav_path.stem)
            if filter_name in filters:
                cutoff_groups[cutoff].append((filter_name, wav_path))

    # Create comparison plots for each cutoff
    for cutoff, files in cutoff_groups.items():
        if verbose:
            print(f"  Generating comparison plot for cutoff={cutoff}")

        comparison_data = []
        for filter_name, wav_path in files:
            samples, sr = read_wav(wav_path)
            freqs, mag_db, _ = compute_frequency_response(samples, sr)
            comparison_data.append((filter_name, freqs, mag_db))

        if comparison_data:
            plot_path = plot_comparison(
                comparison_data,
                plots_dir / "comparison" / f"all_filters_c{cutoff}_magnitude.png",
                title=f"Filter Comparison (fc={cutoff} Hz)"
            )
            plots.append(str(plot_path))

    return plots


# =============================================================================
# Dashboard Generation (imported from dashboard_generator.py)
# =============================================================================

from dashboard_generator import generate_dashboard


# (See dashboard_generator.py for implementation)


# =============================================================================
# Main Execution
# =============================================================================

def run_test_suite(
    runfilters_path: Path,
    output_dir: Path,
    sample_rate: int,
    tests: List[str],
    filters: List[str],
    cutoffs: List[float],
    resonances: List[float],
    oversamples: List[int],
    skip_existing: bool,
    no_plots: bool,
    verbose: bool
) -> MetricsSummary:
    """Run the complete test suite.

    Args:
        runfilters_path: Path to RunFilters.exe
        output_dir: Base output directory
        sample_rate: Sample rate in Hz
        tests: List of test names to run
        filters: List of filter names to include
        cutoffs: List of cutoff frequencies
        resonances: List of resonance values
        oversamples: List of oversample factors
        skip_existing: Skip tests with existing outputs
        no_plots: Skip plot generation
        verbose: Print progress

    Returns:
        MetricsSummary object with all results
    """
    # Create run directory with timestamp
    run_id = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    run_dir = output_dir / run_id

    inputs_dir = run_dir / "inputs"
    wav_dir = run_dir / "wav"
    plots_dir = run_dir / "plots"
    metrics_dir = run_dir / "metrics"

    for d in [inputs_dir, wav_dir, plots_dir, metrics_dir]:
        d.mkdir(parents=True, exist_ok=True)

    all_results = []

    # Run requested tests
    if "linear" in tests:
        if verbose:
            print("Running linear response tests...")
        results = run_linear_response_test(
            runfilters_path, inputs_dir, wav_dir, plots_dir, sample_rate,
            cutoffs, filters, oversamples, skip_existing, no_plots, verbose
        )
        all_results.extend(results)

        # Save test-specific metrics
        with open(metrics_dir / "linear_response.json", 'w') as f:
            json.dump([asdict(r) for r in results], f, indent=2)

    if "resonance" in tests:
        if verbose:
            print("Running resonance tests...")
        results = run_resonance_test(
            runfilters_path, inputs_dir, wav_dir, plots_dir, sample_rate,
            cutoffs, filters, skip_existing, no_plots, verbose
        )
        all_results.extend(results)

        with open(metrics_dir / "resonance.json", 'w') as f:
            json.dump([asdict(r) for r in results], f, indent=2)

    if "selfoscillation" in tests:
        if verbose:
            print("Running self-oscillation tests...")
        results = run_selfoscillation_test(
            runfilters_path, inputs_dir, wav_dir, plots_dir, sample_rate,
            cutoffs, filters, skip_existing, no_plots, verbose
        )
        all_results.extend(results)

        with open(metrics_dir / "selfoscillation.json", 'w') as f:
            json.dump([asdict(r) for r in results], f, indent=2)

    if "thd" in tests:
        if verbose:
            print("Running THD tests...")
        results = run_thd_test(
            runfilters_path, inputs_dir, wav_dir, plots_dir, sample_rate,
            filters, skip_existing, no_plots, verbose
        )
        all_results.extend(results)

        with open(metrics_dir / "thd.json", 'w') as f:
            json.dump([asdict(r) for r in results], f, indent=2)

    if "imd" in tests:
        if verbose:
            print("Running IMD tests...")
        results = run_imd_test(
            runfilters_path, inputs_dir, wav_dir, plots_dir, sample_rate,
            filters, skip_existing, no_plots, verbose
        )
        all_results.extend(results)

        with open(metrics_dir / "imd.json", 'w') as f:
            json.dump([asdict(r) for r in results], f, indent=2)

    if "aliasing" in tests:
        if verbose:
            print("Running aliasing tests...")
        results = run_aliasing_test(
            runfilters_path, inputs_dir, wav_dir, plots_dir, sample_rate,
            filters, oversamples, skip_existing, no_plots, verbose
        )
        all_results.extend(results)

        with open(metrics_dir / "aliasing.json", 'w') as f:
            json.dump([asdict(r) for r in results], f, indent=2)

    if "step" in tests:
        if verbose:
            print("Running step response tests...")
        results = run_step_test(
            runfilters_path, inputs_dir, wav_dir, plots_dir, sample_rate,
            filters, skip_existing, no_plots, verbose
        )
        all_results.extend(results)

        with open(metrics_dir / "step.json", 'w') as f:
            json.dump([asdict(r) for r in results], f, indent=2)

    if "noise" in tests:
        if verbose:
            print("Running noise shaping tests...")
        results = run_noise_test(
            runfilters_path, inputs_dir, wav_dir, plots_dir, sample_rate,
            filters, skip_existing, no_plots, verbose
        )
        all_results.extend(results)

        with open(metrics_dir / "noise.json", 'w') as f:
            json.dump([asdict(r) for r in results], f, indent=2)

    # Generate comparison plots
    if not no_plots and "linear" in tests:
        if verbose:
            print("Generating comparison plots...")
        generate_comparison_plots(wav_dir, plots_dir, sample_rate, filters, verbose)

    # Create summary
    summary = MetricsSummary(
        run_id=run_id,
        timestamp=datetime.now().isoformat(),
        sample_rate=sample_rate,
        filters_tested=filters,
        tests_run=tests,
        total_test_cases=len(all_results),
        results=[asdict(r) for r in all_results]
    )

    # Save summary
    with open(metrics_dir / "summary.json", 'w') as f:
        json.dump(asdict(summary), f, indent=2)

    return summary


def print_summary(summary: MetricsSummary):
    """Print a summary table of results."""
    print("\n" + "=" * 60)
    print(f"Filter Verification Summary")
    print(f"Run ID: {summary.run_id}")
    print(f"Sample Rate: {summary.sample_rate} Hz")
    print(f"Filters: {', '.join(summary.filters_tested)}")
    print(f"Tests: {', '.join(summary.tests_run)}")
    print(f"Total Test Cases: {summary.total_test_cases}")
    print("=" * 60)

    # Group results by test type
    by_test = {}
    for result in summary.results:
        test = result['test_case']
        if test not in by_test:
            by_test[test] = []
        by_test[test].append(result)

    for test_name, results in by_test.items():
        print(f"\n{test_name.upper()}:")

        if test_name == "selfoscillation":
            # Special formatting for self-oscillation
            osc_filters = [r['filter_name'] for r in results if r['metrics'].get('oscillating', False)]
            non_osc = [r['filter_name'] for r in results if not r['metrics'].get('oscillating', False)]
            print(f"  Self-oscillating: {', '.join(set(osc_filters)) or 'None'}")
            print(f"  Non-oscillating: {', '.join(set(non_osc)) or 'None'}")

        elif test_name == "thd":
            # THD summary
            for r in results:
                thd = r['metrics'].get('thd_percent', 0)
                level = r['metrics'].get('input_level_dbfs', 0)
                print(f"  {r['filter_name']} @ {level}dBFS: THD={thd:.4f}%")

        elif test_name == "step":
            # Step response summary
            for r in results:
                overshoot = r['metrics'].get('overshoot_pct', 0)
                settling = r['metrics'].get('settling_time_ms', 0)
                print(f"  {r['filter_name']} (Q={r['resonance']}): overshoot={overshoot:.1f}%, settling={settling:.2f}ms")


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description='Filter verification and analysis suite for MoogLadders.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python filter_verification.py --runfilters build/RunFilters.exe
  python filter_verification.py --runfilters build/RunFilters.exe --tests linear,thd --dashboard
  python filter_verification.py --runfilters build/RunFilters.exe --filters Stilson,Huovilainen --verbose
  python filter_verification.py --dashboard-from filter_validation/2026-01-24_194311
        """
    )

    parser.add_argument(
        '--runfilters', '-r',
        type=Path,
        default=None,
        help='Path to RunFilters.exe (required unless using --dashboard-from)'
    )

    parser.add_argument(
        '--samplerate', '-sr',
        type=int,
        default=DEFAULT_SAMPLE_RATE,
        help=f'Sample rate for test signals (default: {DEFAULT_SAMPLE_RATE})'
    )

    parser.add_argument(
        '--outdir', '-o',
        type=Path,
        default=Path('filter_validation'),
        help='Output root directory (default: filter_validation)'
    )

    parser.add_argument(
        '--tests', '-t',
        type=str,
        default='all',
        help=f'Comma-separated test names or "all" (available: {",".join(TEST_NAMES)})'
    )

    parser.add_argument(
        '--filters', '-f',
        type=str,
        default='all',
        help=f'Comma-separated filter names or "all" (available: {",".join(FILTER_NAMES)})'
    )

    parser.add_argument(
        '--os', '-s',
        type=str,
        default='0,4',
        help='Comma-separated oversample factors (default: 0,4)'
    )

    parser.add_argument(
        '--cutoffs', '-c',
        type=str,
        default=None,
        help=f'Comma-separated cutoff frequencies (default: {",".join(map(str, DEFAULT_CUTOFFS))})'
    )

    parser.add_argument(
        '--skip-existing',
        action='store_true',
        help='Skip tests where output already exists'
    )

    parser.add_argument(
        '--no-plots',
        action='store_true',
        help='Skip plot generation (metrics only)'
    )

    parser.add_argument(
        '--verbose', '-v',
        action='store_true',
        help='Print detailed progress'
    )

    parser.add_argument(
        '--dashboard',
        action='store_true',
        help='Generate interactive HTML dashboard after running tests'
    )

    parser.add_argument(
        '--dashboard-from',
        type=Path,
        default=None,
        metavar='RUN_DIR',
        help='Generate dashboard from existing run directory (e.g., filter_validation/2026-01-24_194311)'
    )

    args = parser.parse_args()

    # Handle dashboard-from mode (generate dashboard from existing results)
    if args.dashboard_from:
        if not args.dashboard_from.exists():
            print(f"Error: Run directory not found: {args.dashboard_from}")
            sys.exit(1)

        print(f"Generating dashboard from: {args.dashboard_from}")
        dashboard_path = generate_dashboard(args.dashboard_from, verbose=args.verbose)
        print(f"\nDashboard generated: {dashboard_path}")
        return

    # Validate RunFilters path for normal operation
    if args.runfilters is None:
        print("Error: --runfilters is required (or use --dashboard-from to generate from existing results)")
        sys.exit(1)

    if not args.runfilters.exists():
        print(f"Error: RunFilters not found at {args.runfilters}")
        sys.exit(1)

    # Parse test list
    if args.tests.lower() == 'all':
        tests = TEST_NAMES
    else:
        tests = [t.strip() for t in args.tests.split(',')]
        invalid = set(tests) - set(TEST_NAMES)
        if invalid:
            print(f"Error: Unknown tests: {invalid}")
            sys.exit(1)

    # Parse filter list
    if args.filters.lower() == 'all':
        filters = FILTER_NAMES
    else:
        filters = [f.strip() for f in args.filters.split(',')]
        invalid = set(filters) - set(FILTER_NAMES)
        if invalid:
            print(f"Error: Unknown filters: {invalid}")
            sys.exit(1)

    # Parse oversample factors
    oversamples = [int(x) for x in args.os.split(',')]

    # Parse cutoffs
    if args.cutoffs:
        cutoffs = [float(x) for x in args.cutoffs.split(',')]
    else:
        cutoffs = DEFAULT_CUTOFFS

    print(f"Filter Verification Suite")
    print(f"RunFilters: {args.runfilters}")
    print(f"Sample Rate: {args.samplerate} Hz")
    print(f"Tests: {', '.join(tests)}")
    print(f"Filters: {', '.join(filters)}")
    print(f"Oversampling: {oversamples}")
    print(f"Output: {args.outdir}")
    print()

    # Run tests
    summary = run_test_suite(
        runfilters_path=args.runfilters,
        output_dir=args.outdir,
        sample_rate=args.samplerate,
        tests=tests,
        filters=filters,
        cutoffs=cutoffs,
        resonances=DEFAULT_RESONANCES,
        oversamples=oversamples,
        skip_existing=args.skip_existing,
        no_plots=args.no_plots,
        verbose=args.verbose
    )

    # Print summary
    print_summary(summary)

    run_dir = args.outdir / summary.run_id
    print(f"\nResults saved to: {run_dir}")

    # Generate dashboard if requested
    if args.dashboard:
        print("\nGenerating interactive dashboard...")
        dashboard_path = generate_dashboard(run_dir, verbose=args.verbose)
        print(f"Dashboard generated: {dashboard_path}")


if __name__ == '__main__':
    main()
