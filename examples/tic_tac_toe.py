"""井字棋活动服务：网页玩家执 X，LLM 通过活动工具执 O。仅依赖 Python 标准库。"""
import argparse
import hmac
import json
import os
import queue
import threading
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PAGE = """<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>和机器人下井字棋</title>
<style>
body{background:#101820;color:#eaf2f5;font:16px system-ui;margin:0;display:grid;min-height:100vh;place-items:center}
main{width:min(340px,85vw);text-align:center}h1{font-size:24px}p{line-height:1.6;color:#aebec9}
#board{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:24px 0}
button{border:1px solid #43535f;background:#1c2c37;color:#56d7c8;cursor:pointer;border-radius:10px}
#board button{aspect-ratio:1;font:48px system-ui}button:disabled{cursor:default;color:#aebec9}
#error{color:#ff9c9c;min-height:24px}small{color:#aebec9}
</style><main><h1>和机器人下井字棋</h1><p>你执 X，机器人执 O。点击空格落子。</p>
<div id="board"></div><p id="status">读取棋局…</p><p id="error" role="alert"></p>
<small>暂停、恢复和停止，请在聊天中告诉机器人。</small></main>
<script>
const run_id = new URLSearchParams(location.search).get('run_id');
const board = document.getElementById('board'), status = document.getElementById('status');
const error = document.getElementById('error');
let sending = false, reading = false, game;
function render() {
 board.replaceChildren(...game.board.map((mark, index) => {
  const button = document.createElement('button'); button.textContent = mark;
  button.setAttribute('aria-label', `${index + 1} 号格：${mark || '空'}`);
  button.disabled = sending || !!mark || !game.snapshot.running || game.paused || game.turn !== 'X';
  button.onclick = () => move(index + 1); return button;
 })); status.textContent = game.snapshot.status;
}
async function read() {
 if (reading || sending) return; reading = true;
 try {
  const response = await fetch('/board?' + new URLSearchParams({run_id}), {cache:'no-store'});
  const data = await response.json(); if (!response.ok) throw Error(data.error);
  game = data; error.textContent = ''; render();
 } catch (e) {error.textContent = e.message;} finally {reading = false;}
}
async function move(cell) {
 if (sending || reading) return; sending = true; error.textContent = ''; render();
 try {
  const response = await fetch('/move', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({run_id,cell})});
  const data = await response.json(); if (!response.ok) throw Error(data.error);
  game = data;
 } catch (e) {error.textContent = e.message;} finally {sending = false; render(); await read();}
}
read(); setInterval(read, 1000);
</script></html>"""


def winner(board):
    lines = ((0, 1, 2), (3, 4, 5), (6, 7, 8), (0, 3, 6), (1, 4, 7), (2, 5, 8), (0, 4, 8), (2, 4, 6))
    return next((board[a] for a, b, c in lines if board[a] and board[a] == board[b] == board[c]),
                "draw" if all(board) else "")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--public-url", help="玩家能访问的地址，例如 http://服务器IP:8090")
    parser.add_argument("--state", type=Path, default=Path("data/tic-tac-toe.json"))
    args = parser.parse_args()
    token = os.environ.get("ACTIVITY_TOKEN", "")
    if len(token) < 16 or not token.isascii():
        parser.error("请设置至少 16 字符的 ASCII 共享密钥 ACTIVITY_TOKEN")
    base_url = (args.public_url or f"http://127.0.0.1:{args.port}").rstrip("/")
    state = json.loads(args.state.read_text()) if args.state.exists() else {"running": False}
    lock, events = threading.Lock(), queue.Queue()

    def snapshot():
        return {key: state[key] for key in ("running", "run_id", "sequence", "status", "origin") if key in state}

    def view():
        return {"snapshot": snapshot(), "board": state["board"][:], "turn": state["turn"], "paused": state["paused"]}

    def save_and_notify(mode, text):
        state["sequence"] += 1
        args.state.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.state.with_suffix(".tmp")
        temporary.write_text(json.dumps(state, ensure_ascii=False))
        temporary.replace(args.state)
        if text:
            board = "\n".join(" | ".join(state["board"][i + j] or str(i + j + 1) for j in range(3)) for i in (0, 3, 6))
            events.put((state["callback_url"], dict(snapshot(), name=state["name"], mode=mode,
                text=f"{text}\n棋盘（数字为空格编号）：\n{board}")))

    def notify():
        http = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # 活动回调直连，避免系统代理干扰内网通信。
        while True:
            url, event = events.get()
            try:
                request = urllib.request.Request(url, json.dumps(event).encode(),
                    {"Content-Type": "application/json", "Authorization": "Bearer " + token})
                with http.open(request, timeout=10) as response:
                    response.read()
                print(f"回调成功：sequence={event['sequence']} mode={event['mode']}", flush=True)
            except (OSError, ValueError):
                print("回调未确认；棋局已保存，可让机器人查询详情继续", flush=True)

    def move(cell, mark):
        if state["paused"]:
            raise ValueError("棋局已暂停")
        if state["turn"] != mark:
            raise ValueError("还没轮到你")
        if type(cell) is not int or not 1 <= cell <= 9 or state["board"][cell - 1]:
            raise ValueError("请选择 1～9 之间的空格")
        state["board"][cell - 1] = mark
        result = winner(state["board"])
        state["running"] = not bool(result)
        state["turn"] = "O" if mark == "X" else "X"
        state["status"] = {"X": "玩家获胜", "O": "机器人获胜", "draw": "平局"}.get(result,
            "轮到机器人执 O 落子" if mark == "X" else "轮到玩家执 X 落子")
        save_and_notify("force" if result or mark == "X" else "silent", state["status"])

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass  # 不把带参与凭据的 URL 写入访问日志。

        def reply(self, value, code=200):
            html = isinstance(value, str)
            data = (value if html else json.dumps(value, ensure_ascii=False)).encode()
            self.send_response(code)
            self.send_header("Content-Type", ("text/html" if html else "application/json") + "; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            if url.path in ("/", "/play"):
                self.reply(PAGE)
                return
            with lock:
                run_id = urllib.parse.parse_qs(url.query).get("run_id", [""])[0]
                if url.path == "/board" and run_id and run_id == state.get("run_id"):
                    self.reply(view())
                else:
                    self.reply({"error": "请让机器人启动井字棋，并打开它返回的链接"}, 404)

        def do_POST(self):
            if self.path != "/move" and not hmac.compare_digest(
                    self.headers.get("Authorization", "").encode(), ("Bearer " + token).encode()):
                self.reply({"error": "认证失败"}, 401)
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 65536:
                    raise ValueError("请求长度无效")
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict):
                    raise ValueError("请求必须是 JSON 对象")
                with lock:
                    if self.path == "/status":
                        result = snapshot()
                    elif self.path == "/start":
                        if not all(body.get(key) for key in ("run_id", "origin", "name", "callback_url")):
                            raise ValueError("缺少启动字段")
                        if state.get("running") and body["run_id"] != state["run_id"]:
                            raise ValueError("已有一局正在进行")
                        if body["run_id"] != state.get("run_id"):
                            state.clear()
                            state.update({key: body[key] for key in ("run_id", "origin", "name", "callback_url")})
                            state.update(running=True, sequence=-1, status="轮到玩家执 X 落子", board=[""] * 9, turn="X", paused=False)
                            save_and_notify("silent", "")
                        result = dict(view(), success=state["running"],
                            url=base_url + "/play?" + urllib.parse.urlencode({"run_id": state["run_id"]}))
                    else:
                        if not body.get("run_id") or body["run_id"] != state.get("run_id"):
                            raise ValueError("棋局不存在或链接已失效")
                        if self.path == "/details":
                            result = dict(view(), commands=[
                                {"instruction": "落子", "parameters": {"cell": 5}},
                                {"instruction": "暂停"}, {"instruction": "恢复"}],
                                rules="玩家 X 先手，机器人 O 后手；cell 为从左到右、从上到下的 1～9。只能在 O 回合调用落子。")
                        elif self.path == "/stop":
                            if state["running"]:
                                state.update(running=False, status="棋局已停止")
                                save_and_notify("force", state["status"])
                            result = dict(view(), success=True)
                        elif self.path in ("/move", "/command"):
                            if not state["running"]:
                                raise ValueError("本局已结束，请让机器人开始新局")
                            if self.path == "/move":
                                move(body.get("cell"), "X")
                            else:
                                command = body.get("arguments", {})
                                instruction = command.get("instruction", "").strip()
                                if instruction in ("落子", "move"):
                                    move(command.get("parameters", {}).get("cell"), "O")
                                elif instruction in ("暂停", "pause", "恢复", "resume"):
                                    paused = instruction in ("暂停", "pause")
                                    if state["paused"] != paused:
                                        state["paused"] = paused
                                        state["status"] = "棋局已暂停" if paused else (
                                            "轮到机器人执 O 落子" if state["turn"] == "O" else "轮到玩家执 X 落子")
                                        mode = "silent" if paused else "force" if state["turn"] == "O" else "judge"
                                        save_and_notify(mode, state["status"])
                                else:
                                    raise ValueError("支持：落子（parameters.cell 为 1～9）、暂停、恢复")
                            result = dict(view(), success=True)
                        else:
                            self.reply({"error": "接口不存在"}, 404)
                            return
            except (ValueError, TypeError, AttributeError, UnicodeError) as error:
                self.reply({"error": str(error)}, 400)
                return
            self.reply(result)

    threading.Thread(target=notify, daemon=True).start()
    print(f"井字棋服务监听 0.0.0.0:{args.port}；玩家访问地址 {base_url}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
