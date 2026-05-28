import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from Backend.services import tts_service


class TTSServiceTest(unittest.TestCase):
    def test_synthesize_requests_wav_and_writes_wav_file(self):
        response = Mock()
        response.status_code = 200
        response.content = b"RIFF....WAVEfmt "

        with tempfile.TemporaryDirectory() as tmpdir:
            with patch.object(
                tts_service.requests, "post", return_value=response
            ) as post:
                result = tts_service.synthesize(
                    region="japanwest",
                    subscription_key="secret",
                    text="你好",
                    output_dir=Path(tmpdir),
                    voice="zh-CN-XiaoxiaoNeural",
                )

            self.assertTrue(result["success"])
            self.assertEqual(result["audio_format"], "wav")
            self.assertEqual(result["content_type"], "audio/wav")
            self.assertTrue(result["audio_filename"].endswith(".wav"))
            self.assertTrue((Path(tmpdir) / result["audio_filename"]).exists())

        headers = post.call_args.kwargs["headers"]
        self.assertEqual(
            headers["X-Microsoft-OutputFormat"],
            tts_service.AZURE_TTS_OUTPUT_FORMAT,
        )
        self.assertNotIn("mp3", headers["X-Microsoft-OutputFormat"])


if __name__ == "__main__":
    unittest.main()
