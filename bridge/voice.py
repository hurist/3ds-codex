"""Local Mandarin transcription for signed 16-bit microphone samples."""

from array import array
from math import isqrt
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
from threading import BoundedSemaphore
from time import monotonic
import wave


MIC_RATE = 16364
WHISPER_RATE = 16000
MAX_AUDIO_BYTES = MIC_RATE * 2 * 30


class VoiceError(Exception):
    pass


def prepare_audio(pcm):
    samples = array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    if len(samples) < MIC_RATE // 3:
        raise VoiceError("录音太短，请说完一句话再结束。")

    # Remove the DC component and low-frequency rumble before measuring the signal.
    signal = array("h")
    previous = samples[0]
    filtered = 0.0
    for sample in samples:
        filtered = sample - previous + 0.965 * filtered
        signal.append(max(-32768, min(32767, int(filtered))))
        previous = sample

    frame_size = MIC_RATE // 50
    levels = []
    for start in range(0, len(signal), frame_size):
        frame = signal[start:start + frame_size]
        levels.append(isqrt(sum(value * value for value in frame) // len(frame)))
    threshold = max(30, min(240, max(levels) // 6))
    active = [i for i, level in enumerate(levels) if level >= threshold]
    if len(active) < 4:
        raise VoiceError(f"录音音量太低（峰值 {max(levels)}），请靠近麦克风重试。")

    start = max(0, (active[0] - 10) * frame_size)
    end = min(len(signal), (active[-1] + 11) * frame_size)
    signal = signal[start:end]
    typical = sorted(levels[i] for i in active)[len(active) * 4 // 5]
    peak = max(map(abs, signal))
    gain = min(8, 2400 / typical, 30000 / peak)
    resampled = array("h")
    for i in range((len(signal) - 1) * WHISPER_RATE // MIC_RATE):
        index, fraction = divmod(i * MIC_RATE, WHISPER_RATE)
        value = (signal[index] * (WHISPER_RATE - fraction) + signal[index + 1] * fraction) / WHISPER_RATE
        resampled.append(round(value * gain))
    if sys.byteorder != "little":
        resampled.byteswap()
    return resampled, peak, typical, gain


class Transcriber:
    def __init__(self, executable, model, opencc_config=None):
        self.executable = executable
        self.model = Path(model).expanduser().resolve() if model else None
        self.slot = BoundedSemaphore(1)
        self.opencc = shutil.which("opencc")
        self.opencc_config = opencc_config or "t2s.json"

    def transcribe(self, pcm):
        if self.model is None or not self.model.is_file():
            raise VoiceError("电脑端还没有配置本地语音模型。")
        if not self.slot.acquire(blocking=False):
            raise VoiceError("电脑正在转写另一段录音，请稍后重试。")
        try:
            return self.run(pcm)
        finally:
            self.slot.release()

    def run(self, pcm):
        started = monotonic()
        resampled, peak, rms, gain = prepare_audio(pcm)
        print(f"语音输入：{len(pcm) // 2 * 1000 // MIC_RATE} ms，"
              f"处理后 {len(resampled) * 1000 // WHISPER_RATE} ms，"
              f"峰值 {peak}，音量 {rms}，增益 {gain:.1f}×", flush=True)

        with TemporaryDirectory(prefix="3ds-codex-voice-") as directory:
            audio = Path(directory) / "audio.wav"
            output = Path(directory) / "text"
            with wave.open(str(audio), "wb") as wav:
                wav.setparams((1, 2, WHISPER_RATE, 0, "NONE", "not compressed"))
                wav.writeframes(resampled.tobytes())
            try:
                subprocess.run(
                    [self.executable, "-m", str(self.model), "-f", str(audio),
                     "-l", "zh", "--suppress-nst", "-ng", "-nt",
                     "-t", "6", "-otxt", "-of", str(output), "-np"],
                    check=True, timeout=55, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
                transcript = output.with_suffix(".txt").read_text(encoding="utf-8").strip()
            except subprocess.TimeoutExpired as error:
                raise VoiceError("本地转写超时，请缩短录音后重试。") from error
            except (OSError, subprocess.CalledProcessError) as error:
                raise VoiceError("本地转写失败，请检查 Whisper 程序和模型。") from error
        if not transcript:
            raise VoiceError("收到录音，但没有识别出可用文字，请重新录音。")
        if self.opencc:
            try:
                transcript = subprocess.run(
                    [self.opencc, "-c", self.opencc_config], input=transcript,
                    encoding="utf-8", check=True, timeout=3, stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                ).stdout.strip() or transcript
            except (OSError, subprocess.SubprocessError):
                pass
        print(f"语音转写完成：{monotonic() - started:.1f} 秒，{len(transcript)} 字符", flush=True)
        return transcript
