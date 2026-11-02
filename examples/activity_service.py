"""实时活动 HTTP 协议示例；只依赖 Python 标准库，不实现具体游戏。"""
import argparse
import hmac
import json
import os
import threading
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8089)
    parser.add_argument("--public-url", default="http://127.0.0.1:8089")
    parser.add_argument("--state", type=Path, default=Path("activity-demo.json"))
    args = parser.parse_args()
    token = os.environ.get("ACTIVITY_TOKEN", "")
    if len(token) < 16:
        parser.error("请设置至少 16 字符的 ACTIVITY_TOKEN")
    lock = threading.RLock()
    state = json.loads(args.state.read_text()) if args.state.exists() else {"running": False}

    def save():
        args.state.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.state.with_suffix(args.state.suffix + ".tmp")
        temporary.write_text(json.dumps(state, ensure_ascii=False))
        temporary.replace(args.state)

    def snapshot():
        return {key: value for key, value in state.items() if key not in ("callback_url", "name")}

    def notify(event, callback_url):
        request = urllib.request.Request(callback_url, json.dumps(event).encode(),
            {"Content-Type": "application/json", "Authorization": "Bearer " + token})
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                response.read()
        except (urllib.error.URLError, TimeoutError):
            # 示例不实现可靠通知队列；主程序仍可查询持久化状态。
            print("回调未确认；主程序可通过状态查询恢复进展")

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass  # 示例页面 URL 含参与凭据，不写访问日志。

        def respond(self, data, code=200):
            content = json.dumps(data, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)

        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            run_id = urllib.parse.parse_qs(parsed.query).get("run_id", [""])[0]
            with lock:
                valid = state.get("running") and hmac.compare_digest(run_id, state.get("run_id", ""))
            if parsed.path != "/play" or not valid:
                self.respond({"error": "活动不存在或已结束"}, 404)
                return
            # run_id 来自主程序 UUID，并经 JSON 编码后放入脚本。
            page = """<!doctype html><meta charset="utf-8"><title>活动协议示例</title>
<main style="max-width:640px;margin:60px auto;font:18px system-ui">
<h1>实时活动协议示例</h1><p>这个页面模拟独立服务中的用户交互，不通过群聊操作。</p>
<button onclick="send(false)">更新进度并通知机器人</button>
<button onclick="send(true)">结束活动并通知机器人</button><p id="result"></p></main>
<script>const runId = RUN_ID;
async function send(finished) {
 const response = await fetch('/demo/update', {method:'POST',headers:{'Content-Type':'application/json'},
 body: JSON.stringify({run_id:runId,finished})});
 document.getElementById('result').textContent = response.ok ? '已更新' : '活动已结束或更新失败';
}</script>""".replace("RUN_ID", json.dumps(run_id))
            content = page.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)

        def do_POST(self):
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length < 0 or length > 128 * 1024:
                    self.respond({"error": "请求过大"}, 413)
                    return
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict):
                    raise ValueError()
            except (ValueError, json.JSONDecodeError):
                self.respond({"error": "无效 JSON"}, 400)
                return
            demo = self.path == "/demo/update"
            if not demo and not hmac.compare_digest(self.headers.get("Authorization", ""), "Bearer " + token):
                self.respond({"error": "认证失败"}, 401)
                return
            event = None
            callback_url = ""
            with lock:
                if self.path == "/status":
                    result = snapshot()
                elif self.path == "/start":
                    if not body.get("run_id") or not body.get("origin") or not body.get("callback_url"):
                        self.respond({"error": "缺少启动字段"}, 400)
                        return
                    if state.get("running"):
                        if body["run_id"] != state["run_id"]:
                            self.respond({"error": "活动已经运行"}, 409)
                            return
                    elif body["run_id"] == state.get("run_id"):
                        self.respond({"error": "此运行已经结束", "snapshot": snapshot()})
                        return
                    else:
                        state.clear()
                        state.update(running=True, run_id=body["run_id"], sequence=0,
                            status="等待进入示例页面", origin=body["origin"],
                            name=body["name"], callback_url=body["callback_url"])
                        save()
                    result = {"success": True, "snapshot": snapshot(),
                        "url": args.public_url.rstrip("/") + "/play?" + urllib.parse.urlencode({"run_id": state["run_id"]})}
                elif self.path in ("/details", "/command", "/stop", "/demo/update"):
                    if not state.get("running") or body.get("run_id") != state.get("run_id"):
                        self.respond({"error": "活动未运行或启动标识不匹配"}, 409)
                        return
                    if self.path == "/details":
                        result = {"snapshot": snapshot(), "commands": "可用 instruction 指挥本示例；停止工具结束活动"}
                    else:
                        finished = self.path == "/stop" or (demo and body.get("finished") is True)
                        state["sequence"] += 1
                        state["running"] = not finished
                        state["status"] = "活动已结束" if finished else (
                            "用户已更新进度" if demo else "收到指令：" + str(body.get("arguments", {}).get("instruction", ""))[:300])
                        save()
                        result = {"success": True, "snapshot": snapshot()}
                        event = dict(snapshot(), name=state["name"], text=state["status"], mode="force" if demo else "silent")
                        callback_url = state["callback_url"]
                else:
                    self.respond({"error": "接口不存在"}, 404)
                    return
            self.respond(result)
            if event:
                threading.Thread(target=notify, args=(event, callback_url), daemon=True).start()

    print(f"示例活动服务监听 0.0.0.0:{args.port}")
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
