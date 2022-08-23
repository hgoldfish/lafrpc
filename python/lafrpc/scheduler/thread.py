#
import threading
from .base import BaseScheduler


class ThreadScheduler(BaseScheduler):
    def get_current(self):
        current_id = threading.current_thread().ident
        for task_id, task in self.tasks.items():
            if task.ident == current_id:
                return task
        return None

    def get_current_id(self):
        return threading.current_thread().ident
