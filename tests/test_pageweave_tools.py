import io
import json
import types
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import MagicMock, patch
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlsplit


def load_tool(name):
    path = Path(__file__).resolve().parents[1] / "agentTools" / f"{name}.json"
    definition = json.loads(path.read_text(encoding="utf-8"))
    module = types.ModuleType(name)
    exec(compile(definition["scriptContent"], str(path), "exec"), module.__dict__)
    return module


class PageWeaveToolTests(unittest.TestCase):
    def setUp(self):
        self.reader = load_tool("fetch_webpage")
        self.search = load_tool("search_web")

    def test_url_normalization_preserves_query_and_fragment(self):
        cases = {
            " example.com/article?q=one#section ": "https://example.com/article?q=one#section",
            "HTTP://example.com/article": "http://example.com/article",
            "//example.com/article": "https://example.com/article",
            "example.com:8080/article": "https://example.com:8080/article",
            "[2001:db8::1]/article": "https://[2001:db8::1]/article",
        }
        for value, expected in cases.items():
            with self.subTest(value=value):
                self.assertEqual(self.reader.normalize_url(value), expected)

    def test_invalid_urls_are_rejected_before_request(self):
        values = [
            None,
            "",
            "ftp://example.com/article",
            "file:///etc/passwd",
            "javascript:alert(1)",
            "mailto:user@example.com",
            "https:example.com",
            "https://user:password@example.com",
            "https://",
            "https://example.com:0",
            "example.com:99999/article",
            "https://example.com/a b",
            "https://example.com/a\nb",
            "https://example.com\\article",
        ]
        with patch.object(self.reader, "urlopen") as request:
            for value in values:
                with self.subTest(value=value), self.assertRaises(ValueError):
                    self.reader.build_payload({"url": value})
            request.assert_not_called()

    def test_reader_defaults_preserve_links_and_use_full_scope(self):
        payload = self.reader.build_payload({"url": "example.com"})
        self.assertEqual(payload["url"], "https://example.com")
        self.assertEqual(payload["content_scope"], "full")
        self.assertEqual(payload["format"], "markdown")
        self.assertTrue(payload["include_links"])
        self.assertFalse(payload["include_images"])
        self.assertEqual(payload["max_chars"], 12000)

    def test_reader_filter_options_remain_compatible(self):
        payload = self.reader.build_payload(
            {
                "url": "https://example.com",
                "content_scope": "main",
                "remove_images": False,
                "remove_links": True,
                "max_chars": 1000,
            }
        )
        self.assertEqual(payload["content_scope"], "main")
        self.assertTrue(payload["include_images"])
        self.assertFalse(payload["include_links"])
        self.assertEqual(payload["max_chars"], 1000)

    def test_invalid_limits_and_flags_are_rejected(self):
        for limit in [0, 50001, True, 1.5, "12000", None]:
            with self.subTest(limit=limit), self.assertRaises(ValueError):
                self.reader.build_payload({"url": "example.com", "max_chars": limit})
        for extra in [{"content_scope": "article"}, {"remove_images": 1}, {"remove_links": None}]:
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                self.reader.build_payload({"url": "example.com", **extra})

    def test_search_encodes_query_without_injecting_url_parameters(self):
        query = "中文 C++ & Python? #资料"
        payload = self.search.build_payload({"query": f" {query} "})
        parsed = urlsplit(payload["url"])
        self.assertEqual(parsed.scheme, "https")
        self.assertEqual(parsed.netloc, "www.bing.com")
        self.assertEqual(parsed.path, "/search")
        self.assertEqual(parse_qs(parsed.query), {"q": [query]})
        self.assertEqual(parsed.fragment, "")
        self.assertEqual(payload["wait_for_selector"], "#b_results")
        self.assertEqual(payload["content_scope"], "full")
        self.assertTrue(payload["include_links"])

    def test_invalid_search_queries_are_rejected(self):
        for value in [None, "", " ", 123, "x" * 501]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.search.build_payload({"query": value})

    def test_request_posts_utf8_json_to_configured_endpoint(self):
        payload = self.reader.build_payload({"url": "https://example.com/资料"})
        response = MagicMock()
        response.__enter__.return_value = response
        response.read.return_value = json.dumps({"content": "中文正文"}).encode("utf-8")
        with patch.object(self.reader, "PAGEWEAVE_URL", "http://127.0.0.1:7779/extract"):
            with patch.object(self.reader, "urlopen", return_value=response) as request:
                result = self.reader.request_pageweave(payload)
        sent = request.call_args.args[0]
        self.assertEqual(sent.full_url, "http://127.0.0.1:7779/extract")
        self.assertEqual(sent.get_method(), "POST")
        self.assertEqual(json.loads(sent.data), payload)
        self.assertEqual(request.call_args.kwargs["timeout"], 30)
        self.assertEqual(result["content"], "中文正文")

    def test_http_error_keeps_service_details_without_retry(self):
        body = json.dumps(
            {
                "request_id": "req-test",
                "error": {"code": "extraction_timeout", "message": "目标元素未出现"},
            }
        ).encode("utf-8")
        error = HTTPError(self.reader.PAGEWEAVE_URL, 504, "Gateway Timeout", None, io.BytesIO(body))
        with patch.object(self.reader, "urlopen", side_effect=error) as request:
            with self.assertRaisesRegex(RuntimeError, "HTTP 504.*extraction_timeout.*目标元素未出现.*req-test"):
                self.reader.request_pageweave({"url": "https://example.com"})
        self.assertEqual(request.call_count, 1)

    def test_transport_errors_are_returned_without_traceback(self):
        for error in [URLError("connection refused"), TimeoutError("timed out")]:
            with self.subTest(error=error), patch.object(self.reader, "urlopen", side_effect=error):
                with self.assertRaisesRegex(RuntimeError, "连接失败或超时"):
                    self.reader.request_pageweave({"url": "https://example.com"})

    def test_invalid_or_empty_responses_fail(self):
        for body in [b"<html>error</html>", b"[]", b"{}", b'{"content":" "}']:
            response = MagicMock()
            response.__enter__.return_value = response
            response.read.return_value = body
            with self.subTest(body=body), patch.object(self.reader, "urlopen", return_value=response):
                with self.assertRaises(RuntimeError):
                    self.reader.request_pageweave({"url": "https://example.com"})

    def test_response_and_request_size_limits(self):
        response = MagicMock()
        response.__enter__.return_value = response
        response.read.return_value = b"x" * (self.reader.MAX_RESPONSE_BYTES + 1)
        with patch.object(self.reader, "urlopen", return_value=response):
            with self.assertRaisesRegex(RuntimeError, "1 MiB"):
                self.reader.request_pageweave({"url": "https://example.com"})
        with patch.object(self.reader, "urlopen") as request:
            with self.assertRaisesRegex(ValueError, "16384"):
                self.reader.request_pageweave({"url": "https://example.com/" + "字" * 9000})
            request.assert_not_called()

    def test_output_preserves_source_content_and_warnings(self):
        content = "    code\n\n[来源](https://example.com/source)"
        result = self.reader.format_result(
            {
                "title": "标题",
                "final_url": "https://example.com/final",
                "content": content,
                "truncated": True,
                "warnings": ["render_wait_limit_reached", "fallback_full_content"],
            },
            "https://example.com/input",
        )
        self.assertIn("来源: https://example.com/final", result)
        self.assertIn("并非完整网页", result)
        self.assertIn("动态观察达到上限", result)
        self.assertIn("整页内容兜底", result)
        self.assertTrue(result.endswith(content))

    def test_cli_returns_failure_to_tool_executor(self):
        output = io.StringIO()
        with patch.object(self.reader.sys, "argv", ["tool.py", "input.json"]):
            with patch("builtins.open", return_value=io.StringIO('{"url":"ftp://example.com"}')):
                with redirect_stdout(output), self.assertRaises(SystemExit) as error:
                    self.reader.main()
        self.assertEqual(error.exception.code, 1)
        self.assertIn("工具执行失败", output.getvalue())
        self.assertIn("HTTP 或 HTTPS", output.getvalue())


if __name__ == "__main__":
    unittest.main()
