"""Browse local Codex conversations and submit turns through App Server."""

from dataclasses import dataclass, replace
import json
import os
from pathlib import Path
from queue import Empty, Queue
from secrets import token_hex
import subprocess
from threading import Condition, Lock, Thread
from time import monotonic

from images import Attachment, MARKDOWN_IMAGE, ImagePreviews


class CodexError(Exception):
    pass


class CodexUnavailable(CodexError):
    pass


class ConversationBusy(CodexError):
    pass


@dataclass
class Submission:
    text: str
    turn_id: str = ""
    process: object = None


MAX_REVIEW_BYTES = 112 * 1024
FILE_APPROVAL = "item/fileChange/requestApproval"
COMMAND_APPROVAL = "item/commandExecution/requestApproval"


def visible_diff(diff):
    # Keep line boundaries and make tabs, controls and glyphs outside the
    # handheld font visible. A literal backslash remains distinguishable.
    return "".join(
        char if char == "\n" or (char != "\\" and (
            0x20 <= ord(char) <= 0x7e or 0x3000 <= ord(char) <= 0x303f
            or 0x3400 <= ord(char) <= 0x9fff or 0xff00 <= ord(char) <= 0xffef
        )) else char.encode("unicode_escape").decode("ascii")
        for char in diff
    )


@dataclass
class Approval:
    request_id: int | str
    method: str
    params: dict
    token: str
    decision: str = ""
    resolved: bool = False
    changes: tuple = ()

    @property
    def thread_id(self):
        return self.params.get("threadId")

    @property
    def turn_id(self):
        return self.params.get("turnId")

    @property
    def item_key(self):
        return self.thread_id, self.turn_id, self.params.get("itemId")

    def review(self):
        reason = self.params.get("reason") or "Codex 未提供理由。"
        command = self.params.get("command")
        if self.method == FILE_APPROVAL:
            kind = "file"
            sections = []
            complete = bool(self.changes)
            for change in self.changes:
                action = change["kind"]["type"]
                action_label = {"add": "新增", "delete": "删除", "update": "修改"}.get(action, "变更")
                title = f"{action_label} {json.dumps(change['path'], ensure_ascii=False)}"
                if destination := change["kind"].get("move_path"):
                    title += f" -> {json.dumps(destination)}"
                diff = visible_diff(change["diff"])
                if not diff and action == "update" and not destination:
                    complete = False
                sections.append(f"{title}\n{diff or '［没有可显示的行差异］'}")
            details = "\n\n".join(sections) or "Codex 未提供可核对的文件差异。"
            if grant_root := self.params.get("grantRoot"):
                details += f"\n\n请求会话级写入目录\n{json.dumps(grant_root)}"
                complete = False
        elif (self.method == COMMAND_APPROVAL
              and self.params.get("kind", "command") == "command"
              and self.params.get("networkApprovalContext") is None):
            kind = "command"
            complete = bool(command)
            details = f"命令\n{json.dumps(command)}\n\n工作目录\n{json.dumps(self.params.get('cwd'))}"
        else:
            details = f"请在支持此类审批的客户端处理。\n\n理由\n{visible_diff(reason)}"
            if len(details.encode()) > MAX_REVIEW_BYTES:
                details = "内容超过掌机显示容量。"
            return "other", details, ()

        details += f"\n\n理由\n{visible_diff(reason)}"
        if len(details.encode()) > MAX_REVIEW_BYTES:
            details = "内容超过掌机显示容量。"
            complete = False
        if not complete:
            details += "\n\n无法完整核对，或请求超出了单次授权范围，因此无法批准。"

        options = self.params.get("availableDecisions")
        if options is None:
            options = ("accept", "decline")
        rejection = "decline" if "decline" in options else "cancel"
        choices = tuple(choice for choice in ("accept", rejection)
                        if choice in options and (choice != "accept" or complete))
        return kind, details, choices


@dataclass
class InputRequest:
    request_id: object
    token: str
    thread_id: str
    turn_id: str
    questions: list
    response: object = None
    resolved: bool = False


def parse_answers(questions, raw):
    answers = {}
    for question in questions:
        head, sep, raw = raw.partition(b"\n")
        if not sep or not head[1:].isdigit() or len(head) > 5 or question.get("isSecret"):
            raise ConversationBusy("无法在掌机上提交这份回答。")
        value = int(head[1:])
        options = question.get("options") or []
        if head[:1] == b"o" and value < len(options):
            answer = options[value]["label"]
        elif head[:1] == b"t" and (not options or question.get("isOther")) and 0 < value <= len(raw):
            try:
                answer = raw[:value].decode("utf-8")
            except UnicodeDecodeError as error:
                raise ConversationBusy("回答文字不完整。") from error
            raw = raw[value:]
            if not answer.strip():
                raise ConversationBusy("请先填写答案。")
        else:
            raise ConversationBusy("回答格式无效。")
        answers[question["id"]] = {"answers": [answer]}
    if raw:
        raise ConversationBusy("回答格式无效。")
    return answers


class AppServer:
    def __init__(self):
        self.process = None
        self.replies = None
        self.sequence = 0
        self.events = Condition()
        self.turn_results = {}
        self.approvals = {}
        self.inputs = {}
        self.input_receipts = {}
        self.decisions = {}
        self.file_changes = {}

    def read_replies(self, stdout, replies):
        try:
            for line in stdout:
                message = json.loads(line)
                if "id" in message and "method" not in message:
                    replies.put(message)
                elif "method" in message:
                    self.record_event(message)
        finally:
            replies.put(None)

    def record_event(self, message):
        method = message["method"]
        params = message.get("params") or {}
        with self.events:
            if method == "turn/completed":
                turn = params["turn"]
                key = (params["threadId"], turn["id"])
                self.turn_results[key] = turn["status"]
                for request_id, approval in list(self.approvals.items()):
                    if (approval.thread_id, approval.turn_id) == key:
                        del self.approvals[request_id]
                for request_id, question in list(self.inputs.items()):
                    if (question.thread_id, question.turn_id) == key:
                        del self.inputs[request_id]
                for item_key in list(self.file_changes):
                    if item_key[:2] == key:
                        del self.file_changes[item_key]
                self.events.notify_all()
            elif method in ("item/started", "item/completed") and params.get("item", {}).get("type") == "fileChange":
                item = params["item"]
                item_key = (params["threadId"], params["turnId"], item["id"])
                if method == "item/started":
                    changes = tuple(item["changes"])
                    self.file_changes[item_key] = changes
                    for approval in self.approvals.values():
                        if (approval.method == FILE_APPROVAL and approval.item_key == item_key
                                and not approval.decision and approval.changes != changes):
                            approval.changes = changes
                            approval.token = token_hex(16)
                else:
                    self.file_changes.pop(item_key, None)
                    for request_id, approval in list(self.approvals.items()):
                        if approval.method == FILE_APPROVAL and approval.item_key == item_key:
                            del self.approvals[request_id]
                    self.events.notify_all()
            elif method.endswith("/requestApproval") and "id" in message:
                request_id = message["id"]
                approval = Approval(request_id, method, params, token_hex(16))
                if method == FILE_APPROVAL:
                    approval.changes = self.file_changes.get(approval.item_key, ())
                self.approvals[request_id] = approval
            elif method == "item/tool/requestUserInput" and "id" in message:
                request_id = message["id"]
                self.inputs[request_id] = InputRequest(
                    request_id, token_hex(16), params["threadId"], params["turnId"], params["questions"],
                )
                self.events.notify_all()
            elif method == "serverRequest/resolved":
                question = self.inputs.pop(params.get("requestId"), None)
                if question:
                    question.resolved = True
                approval = self.approvals.pop(params.get("requestId"), None)
                if approval:
                    approval.resolved = True
                self.events.notify_all()

    def turn_status(self, thread_id, turn_id):
        with self.events:
            key = (thread_id, turn_id)
            if key in self.turn_results:
                return self.turn_results[key]
            if any((a.thread_id, a.turn_id) == key for a in self.approvals.values()):
                return "approval"
            if any((q.thread_id, q.turn_id) == key for q in self.inputs.values()):
                return "input"
            return "running"

    def pending_input(self, thread_id):
        with self.events:
            if self.process is None or self.process.poll() is not None:
                raise KeyError("提问会话已结束。")
            return next((q for q in self.inputs.values() if q.thread_id == thread_id), None)

    def answer_input(self, thread_id, token, raw):
        with self.events:
            if self.process is None or self.process.poll() is not None:
                raise KeyError("提问会话已结束。")
            question = self.input_receipts.get(token)
            if question is None:
                question = next((q for q in self.inputs.values() if q.token == token), None)
            if question is None or question.thread_id != thread_id:
                raise KeyError("这条提问已失效，请返回对话。")
            answers = parse_answers(question.questions, raw)
            if question.response is None:
                question.response = answers
                self.input_receipts[token] = question
                try:
                    self.send({"id": question.request_id, "result": {"answers": answers}})
                except OSError as error:
                    raise CodexUnavailable("无法确认答案是否送达。") from error
            elif question.response != answers:
                raise ConversationBusy("这条提问已经提交过答案。")
            self.events.wait_for(lambda: question.resolved or question.request_id not in self.inputs, timeout=2)
            return question.resolved

    def pending_approval(self, thread_id):
        with self.events:
            if self.process is None or self.process.poll() is not None:
                raise KeyError("审批会话已结束。")
            approval = next((a for a in self.approvals.values()
                             if a.thread_id == thread_id and not a.decision), None)
            return replace(approval) if approval else None

    def decide_approval(self, thread_id, token, decision):
        with self.events:
            if self.process is None or self.process.poll() is not None:
                raise KeyError("审批会话已结束。")
            approval = self.decisions.get(token)
            if approval is None:
                approval = next((a for a in self.approvals.values() if a.token == token), None)
                if approval is None or approval.thread_id != thread_id:
                    raise KeyError("审批已失效，请刷新对话。")
                if decision not in approval.review()[2]:
                    raise ConversationBusy("该审批选项不可用。")
                approval.decision = decision
                self.decisions[token] = approval
                try:
                    self.send({"id": approval.request_id, "result": {"decision": decision}})
                except OSError as error:
                    raise CodexUnavailable("无法确认审批决定是否送达。") from error
            elif approval.thread_id != thread_id or approval.decision != decision:
                raise ConversationBusy("这条审批请求已经处理。")

            self.events.wait_for(
                lambda: approval.resolved or approval.request_id not in self.approvals, timeout=2,
            )
            return approval.resolved

    def send(self, message):
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def request(self, method, params):
        try:
            if self.process is None:
                self.process = subprocess.Popen(
                    ["codex", "app-server", "--listen", "stdio://"],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    text=True,
                    encoding="utf-8",
                )
                self.replies = Queue()
                with self.events:
                    self.turn_results.clear()
                    self.approvals.clear()
                    self.inputs.clear()
                    self.input_receipts.clear()
                    self.decisions.clear()
                    self.file_changes.clear()
                Thread(target=self.read_replies, args=(self.process.stdout, self.replies), daemon=True).start()
                self.exchange("initialize", {"clientInfo": {"name": "3ds-codex", "version": "0.2"}})
                self.send({"method": "initialized"})
            return self.exchange(method, params)
        except (OSError, Empty) as error:
            self.close()
            raise CodexUnavailable("Codex 服务未响应，请检查连接后重试。") from error

    def exchange(self, method, params):
        self.sequence += 1
        self.send({"id": self.sequence, "method": method, "params": params})
        reply = self.replies.get(timeout=5)
        if reply is None or reply.get("id") != self.sequence:
            raise OSError("App Server response was interrupted")
        if "error" in reply:
            raise CodexError(f"Codex 未能完成此操作（{method}）。")
        return reply["result"]

    def close(self):
        process = self.process
        self.process = None
        if process is None:
            return
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        process.stdin.close()
        process.stdout.close()


def desktop_projects(codex_home):
    try:
        state = json.loads((codex_home / ".codex-global-state.json").read_text())
    except FileNotFoundError:
        return {}, {}, set()
    except (OSError, ValueError) as error:
        raise CodexError("无法读取 Codex 桌面项目，请刷新重试。") from error

    local = state.get("local-projects", {})
    projects = {}
    for project_id in dict.fromkeys([*state.get("project-order", []), *local]):
        project = local.get(project_id)
        if project:
            roots = tuple(Path(root).resolve() for root in project.get("rootPaths", []))
            projects[project_id] = (project.get("name") or "未命名项目", roots, [])
    return projects, state.get("thread-project-assignments", {}), set(state.get("projectless-thread-ids", []))


def thread_title(thread):
    return thread.get("name") or thread.get("preview") or "未命名对话"


CHINESE_REPLY = (
    "客户端语言偏好：默认用简体中文，保留代码、命令和专有名词的原文。"
    "若正文指定语言或固定文本，以正文为准。请直接回应接下来的消息正文，无需复述或确认此偏好。"
)


def message_input(text):
    return [{"type": "text", "text": part, "text_elements": []} for part in (CHINESE_REPLY, text)]


def conversation_messages(turns):
    messages = []
    for turn in reversed(turns):
        for item in turn["items"]:
            if item["type"] == "userMessage":
                parts = [part["text"] for part in item["content"] if part["type"] == "text"]
                if len(parts) >= 2 and parts[0] == CHINESE_REPLY:
                    parts.pop(0)
                text = "\n".join(parts)
                if text:
                    messages.append(("你", text))
                for part in item["content"]:
                    if part["type"] in ("localImage", "image"):
                        value = part.get("path") or part.get("url") or ""
                        messages.append(("你", Attachment(value)))
            elif item["type"] == "agentMessage" and item.get("text"):
                messages.append(("Codex", item["text"]))
                for match in MARKDOWN_IMAGE.finditer(item["text"]):
                    messages.append(("Codex", Attachment(match[1].strip("<>"))))
            elif item["type"] == "imageGeneration":
                value = item.get("savedPath") or item.get("result") or ""
                if value or item.get("fileId"):
                    messages.append(("Codex", Attachment(value, not bool(item.get("savedPath")))))
            elif item["type"] == "imageView" and item.get("path"):
                messages.append(("Codex", Attachment(item["path"])))
    return messages


class Library:
    def __init__(self):
        self.codex_home = Path(os.environ.get("CODEX_HOME", Path.home() / ".codex"))
        self.app = AppServer()
        self.image_previews = ImagePreviews()
        self.lock = Lock()
        self.projects = {}
        self.threads = {}
        self.loaded_at = None
        self.history_cursors = {}
        self.submissions = {}
        self.last_submission = {}
        self.control_receipts = {}
        self.stop_requests = set()

    def refresh(self, force=False):
        if not force and self.loaded_at is not None and monotonic() - self.loaded_at < 20:
            return
        projects, assignments, projectless = desktop_projects(self.codex_home)
        threads = {}
        cursor = None
        while True:
            response = self.app.request(
                "thread/list",
                {"cursor": cursor, "limit": 100, "sortKey": "updated_at", "useStateDbOnly": True},
            )
            for thread in response["data"]:
                threads[thread["id"]] = thread
            cursor = response.get("nextCursor")
            if not cursor:
                break

        unassigned = ("未归类", (), [])
        for thread_id, thread in threads.items():
            assignment = assignments.get(thread_id) or {}
            project_id = assignment.get("projectId") if assignment.get("projectKind") == "local" else None
            if project_id in projects:
                projects[project_id][2].append(thread)
                continue
            cwd = Path(thread["cwd"]).resolve()
            match = None
            if thread_id not in projectless:
                match = max(
                    ((len(root.parts), key) for key, (_, roots, _) in projects.items()
                     for root in roots if cwd.is_relative_to(root)),
                    default=None,
                )
            if match:
                projects[match[1]][2].append(thread)
            else:
                unassigned[2].append(thread)
        if unassigned[2]:
            projects["unassigned"] = unassigned
        self.projects, self.threads = projects, threads
        self.loaded_at = monotonic()

    def send_message(self, thread_id, operation, text):
        self.refresh()
        if thread_id not in self.threads:
            raise KeyError("这条对话已不可用。")
        key = (thread_id, operation)
        previous = self.submissions.get(key)
        if previous is not None:
            if previous.text != text:
                raise ConversationBusy("此发送编号已用于另一条消息。")
            return self.submission_status(thread_id, previous)

        if self.latest_status(thread_id) in ("running", "approval", "input", "stopping", "unknown"):
            raise ConversationBusy("请先查看上一条消息的发送状态，或等待当前执行结束。")
        latest = self.app.request("thread/turns/list", {"threadId": thread_id, "limit": 1, "itemsView": "summary"})
        if any(turn["status"] == "inProgress" for turn in latest["data"]):
            raise ConversationBusy("这条对话已有一轮正在执行。")
        resumed = self.app.request("thread/resume", {"threadId": thread_id, "excludeTurns": True})
        if resumed["thread"]["status"]["type"] == "active":
            raise ConversationBusy("这条对话已有一轮正在执行。")

        submission = Submission(text, process=self.app.process)
        self.submissions[key] = submission
        try:
            result = self.app.request("turn/start", {
                "threadId": thread_id,
                "clientUserMessageId": operation,
                "input": message_input(text),
            })
        except CodexUnavailable:
            # The request may have started before its acknowledgment was lost.
            self.last_submission[thread_id] = submission
            return "unknown"
        except CodexError:
            del self.submissions[key]
            raise
        submission.turn_id = result["turn"]["id"]
        self.last_submission[thread_id] = submission
        self.loaded_at = None
        return self.submission_status(thread_id, submission)

    def submission_status(self, thread_id, submission):
        process = submission.process
        if not submission.turn_id or process is not self.app.process or process.poll() is not None:
            return "unknown"
        return self.app.turn_status(thread_id, submission.turn_id)

    def latest_status(self, thread_id):
        submission = self.last_submission.get(thread_id)
        if submission is None:
            return "idle"
        status = self.submission_status(thread_id, submission)
        if status in ("running", "approval", "input") and (thread_id, submission.turn_id) in self.stop_requests:
            return "stopping"
        return status

    def active_turn(self, thread_id):
        if self.latest_status(thread_id) in ("running", "approval", "input", "stopping"):
            return self.last_submission[thread_id].turn_id
        return ""

    def control_turn(self, thread_id, turn_id, action, operation, text):
        key = (thread_id, turn_id, action, operation)
        if key in self.control_receipts:
            original, result = self.control_receipts[key]
            if text != original:
                raise ConversationBusy("此操作编号已用于另一项请求。")
            return result

        if self.active_turn(thread_id) != turn_id or self.latest_status(thread_id) == "stopping":
            raise ConversationBusy("这一轮已不可操作，请刷新对话。")
        self.control_receipts[key] = (text, "unknown")
        try:
            if action == "steer":
                result = self.app.request("turn/steer", {
                    "threadId": thread_id,
                    "expectedTurnId": turn_id,
                    "clientUserMessageId": operation,
                    "input": message_input(text),
                })
                if result["turnId"] != turn_id:
                    return "unknown"
            else:
                self.app.request("turn/interrupt", {"threadId": thread_id, "turnId": turn_id})
                self.stop_requests.add((thread_id, turn_id))
        except CodexUnavailable:
            return "unknown"
        except CodexError:
            del self.control_receipts[key]
            raise
        self.control_receipts[key] = (text, "accepted")
        return "accepted"

    def history(self, thread_id, page):
        if page == 0:
            self.history_cursors[thread_id] = [None]
        cursors = self.history_cursors.get(thread_id, [])
        if page >= len(cursors):
            raise KeyError("该页历史暂不可用，请重新打开对话。")
        result = self.app.request(
            "thread/turns/list",
            {"threadId": thread_id, "cursor": cursors[page], "limit": 2, "itemsView": "full"},
        )
        next_cursor = result.get("nextCursor")
        cursors[page + 1:] = [next_cursor] if next_cursor else []
        status = self.latest_status(thread_id)
        if status == "idle" and result["data"] and result["data"][0]["status"] == "inProgress":
            status = "running"
        return conversation_messages(result["data"]), bool(next_cursor), status
