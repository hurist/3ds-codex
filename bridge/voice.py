"""Local Mandarin transcription for signed 16-bit microphone samples."""

from array import array
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
from threading import BoundedSemaphore
import wave


MIC_RATE = 16364
WHISPER_RATE = 16000
MAX_AUDIO_BYTES = MIC_RATE * 2 * 30


class VoiceError(Exception):
    pass


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
        samples = array("h")
        samples.frombytes(pcm)
        if sys.byteorder != "little":
            samples.byteswap()
        if len(samples) < MIC_RATE // 3:
            raise VoiceError("录音太短，请重新录音。")
        if sum(sample * sample for sample in samples) < len(samples) * 70**2:
            raise VoiceError("没有听到清晰的声音，请检查麦克风。")

        resampled = array("h")
        for i in range((len(samples) - 1) * WHISPER_RATE // MIC_RATE):
            index, fraction = divmod(i * MIC_RATE, WHISPER_RATE)
            value = (samples[index] * (WHISPER_RATE - fraction) +
                     samples[index + 1] * fraction) // WHISPER_RATE
            resampled.append(value)
        if sys.byteorder != "little":
            resampled.byteswap()

        with TemporaryDirectory(prefix="3ds-codex-voice-") as directory:
            audio = Path(directory) / "audio.wav"
            output = Path(directory) / "text"
            with wave.open(str(audio), "wb") as wav:
                wav.setparams((1, 2, WHISPER_RATE, 0, "NONE", "not compressed"))
                wav.writeframes(resampled.tobytes())
            try:
                subprocess.run(
                    [self.executable, "-m", str(self.model), "-f", str(audio),
                     "-l", "zh", "--prompt", "请用简体中文转写普通话。", "-ng",
                     "-t", "4", "-otxt", "-of", str(output), "-np"],
                    check=True, timeout=35, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                )
                transcript = output.with_suffix(".txt").read_text(encoding="utf-8").strip()
            except subprocess.TimeoutExpired as error:
                raise VoiceError("本地转写超时，请缩短录音后重试。") from error
            except (OSError, subprocess.CalledProcessError) as error:
                raise VoiceError("本地转写失败，请检查 Whisper 程序和模型。") from error
        if not transcript:
            raise VoiceError("没有识别出文字，请重新录音。")
        if self.opencc:
            try:
                transcript = subprocess.run(
                    [self.opencc, "-c", self.opencc_config], input=transcript,
                    encoding="utf-8", check=True, timeout=3, stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                ).stdout.strip() or transcript
            except (OSError, subprocess.SubprocessError):
                pass
        return transcript
