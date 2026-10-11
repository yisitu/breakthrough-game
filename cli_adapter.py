"""Runs line-oriented bot subprocesses under a strict per-move deadline."""

import os
import queue
import signal
import subprocess
import threading
import time

MoveResponse = tuple[str | None, float, str | None]


class CLIPlayer:
    """Adapts a persistent CLI bot to the tournament harness.

    Each turn consists of two input lines (board state and move history) and
    exactly one output line. A bot and every process it spawns share a process
    group so a timeout can terminate the entire tree without leaving orphans.
    """

    def __init__(self, name: str, command: str, timeout_sec: float = 0.2):
        self.name = name
        self.command = command
        self.timeout = timeout_sec
        self.process = None

    def start(self):
        """Starts the bot with its cooperative timeout exposed in milliseconds."""
        environment = os.environ.copy()
        environment["BREAKTHROUGH_TIMEOUT_MS"] = str(round(self.timeout * 1000))
        self.process = subprocess.Popen(
            self.command,
            shell=True,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            start_new_session=True,
            env=environment,
        )

    def get_move(self, board_state: str, move_history: str) -> MoveResponse:
        """Requests one move while enforcing the wall-clock deadline.

        Returns:
            A tuple of move text, elapsed seconds, and an optional error. Move
            text is ``None`` whenever an error is present.
        """
        if not self.process:
            return None, 0.0, "Process not started"
        return_code = self.process.poll()
        if return_code is not None:
            return None, 0.0, f"Process exited with code {return_code}"

        payload = f"{board_state}\n{move_history}\n"
        output_queue = queue.Queue()

        def read_output_line():
            # readline() has no portable timeout, so isolate it in a daemon
            # thread and bound the join below.
            try:
                line = self.process.stdout.readline()
                output_queue.put(line)
            except Exception as error:
                output_queue.put(error)

        start_time = time.perf_counter()
        deadline = start_time + self.timeout
        try:
            self.process.stdin.write(payload)
            self.process.stdin.flush()

            reader_thread = threading.Thread(target=read_output_line,
                                             daemon=True)
            reader_thread.start()
            reader_thread.join(timeout=max(0.0, deadline - time.perf_counter()))

            elapsed = time.perf_counter() - start_time

            # The elapsed check catches scheduling delays after readline()
            # completes but before this thread regains control.
            if reader_thread.is_alive() or elapsed > self.timeout:
                self.stop()
                return None, elapsed, f"Timeout (> {self.timeout}s)"

            result = output_queue.get_nowait()
            if isinstance(result, Exception):
                return None, elapsed, f"IO Error: {result}"
            if result == "":
                # EOF means the process exited or deliberately closed stdout;
                # neither can satisfy the persistent bot protocol.
                return_code = self.process.poll()
                if return_code is None:
                    return None, elapsed, "Process closed stdout unexpectedly"
                return None, elapsed, f"Process exited with code {return_code}"

            return_code = self.process.poll()
            if return_code not in (None, 0):
                return None, elapsed, f"Process exited with code {return_code}"
            return result.strip(), elapsed, None

        except Exception as error:
            return None, time.perf_counter() - start_time, str(error)

    def stop(self):
        """Kills the bot process group and closes all harness-owned streams."""
        if not self.process:
            return
        # start_new_session=True makes the shell PID the process-group ID.
        try:
            os.killpg(self.process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        if self.process.poll() is None:
            self.process.wait()
        process_streams = (self.process.stdin, self.process.stdout,
                           self.process.stderr)
        for stream in process_streams:
            if stream:
                stream.close()
