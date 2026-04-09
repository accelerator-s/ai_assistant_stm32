"""Thread-based async job manager for background tasks.

This module provides a lightweight in-process job system suitable for Flask
applications that need to offload slow or device-bound work from request
handlers without introducing external dependencies.

Design goals:
- Non-blocking HTTP request handlers: submit work and return a job id quickly.
- Thread-safe job state tracking.
- Progress / status reporting for polling UIs.
- Conservative resource usage via bounded worker pool.
- Simple cancellation semantics (best-effort).
"""

from __future__ import annotations

import logging
import threading
import time
import traceback
import uuid
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Dict, Optional

logger = logging.getLogger(__name__)


class JobStatus(str, Enum):
    """Lifecycle states for an async job."""

    PENDING = "pending"
    RUNNING = "running"
    SUCCESS = "success"
    ERROR = "error"
    CANCELLED = "cancelled"


@dataclass
class JobContext:
    """Execution context exposed to background tasks.

    The task function can update progress, messages, and attach metadata while
    running. Cancellation is cooperative: tasks should periodically check
    `is_cancelled()` and return early when true.
    """

    job_id: str
    _manager: "AsyncJobManager"

    def set_progress(self, progress: int, message: Optional[str] = None) -> None:
        """Update job progress percentage and optional message."""
        self._manager.update_job(
            self.job_id,
            progress=max(0, min(100, int(progress))),
            message=message,
        )

    def set_message(self, message: str) -> None:
        """Update only the job message."""
        self._manager.update_job(self.job_id, message=message)

    def set_meta(self, **kwargs: Any) -> None:
        """Merge additional metadata into the job record."""
        self._manager.update_job(self.job_id, meta=kwargs)

    def is_cancelled(self) -> bool:
        """Return True when cancellation was requested."""
        job = self._manager.get_job(self.job_id)
        return bool(job and job.cancel_requested)

    def fail(self, message: str, *, error: Optional[str] = None) -> None:
        """Convenience helper to mark the job as failed."""
        self._manager.fail_job(self.job_id, message=message, error=error)


@dataclass
class JobRecord:
    """Internal state for a submitted async job."""

    job_id: str
    name: str
    status: JobStatus = JobStatus.PENDING
    progress: int = 0
    message: str = ""
    result: Any = None
    error: Optional[str] = None
    meta: Dict[str, Any] = field(default_factory=dict)
    created_at: float = field(default_factory=time.time)
    started_at: Optional[float] = None
    finished_at: Optional[float] = None
    cancel_requested: bool = False
    future: Optional[Future] = None

    def to_dict(self) -> Dict[str, Any]:
        """Serialize the job for API responses."""
        return {
            "job_id": self.job_id,
            "name": self.name,
            "status": self.status.value,
            "progress": self.progress,
            "message": self.message,
            "result": self.result,
            "error": self.error,
            "meta": dict(self.meta),
            "created_at": self.created_at,
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "cancel_requested": self.cancel_requested,
        }


class AsyncJobManager:
    """Thread-based background job manager.

    Example:
        manager = AsyncJobManager(max_workers=4)

        def work(ctx: JobContext, device_id: str) -> dict:
            ctx.set_progress(10, "Starting")
            ...
            return {"ok": True}

        job_id = manager.submit("mic-hardware-test", work, "dev-1")
        status = manager.get_job_dict(job_id)
    """

    def __init__(self, max_workers: int = 4, max_jobs: int = 1000) -> None:
        self._executor = ThreadPoolExecutor(
            max_workers=max_workers,
            thread_name_prefix="async-job",
        )
        self._jobs: Dict[str, JobRecord] = {}
        self._lock = threading.RLock()
        self._max_jobs = max(10, int(max_jobs))

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------
    def submit(
        self,
        name: str,
        func: Callable[..., Any],
        *args: Any,
        **kwargs: Any,
    ) -> str:
        """Submit a callable for background execution.

        The callable will be invoked as:
            func(ctx, *args, **kwargs)

        where `ctx` is a JobContext instance.
        """
        job_id = uuid.uuid4().hex
        record = JobRecord(job_id=job_id, name=name)

        with self._lock:
            self._prune_if_needed_unlocked()
            self._jobs[job_id] = record

        future = self._executor.submit(self._run_job, job_id, func, args, kwargs)
        record.future = future
        return job_id

    def get_job(self, job_id: str) -> Optional[JobRecord]:
        """Return the JobRecord or None."""
        with self._lock:
            return self._jobs.get(job_id)

    def get_job_dict(self, job_id: str) -> Optional[Dict[str, Any]]:
        """Return serialized job state or None."""
        job = self.get_job(job_id)
        return job.to_dict() if job else None

    def list_jobs(self) -> Dict[str, Dict[str, Any]]:
        """Return all current jobs as serialized dicts."""
        with self._lock:
            return {job_id: job.to_dict() for job_id, job in self._jobs.items()}

    def cancel_job(self, job_id: str) -> bool:
        """Request cancellation for a job.

        Returns True when the job exists. Cancellation is best-effort:
        - Pending futures may be cancelled immediately.
        - Running jobs must cooperate by checking `ctx.is_cancelled()`.
        """
        with self._lock:
            job = self._jobs.get(job_id)
            if not job:
                return False

            job.cancel_requested = True
            if job.status == JobStatus.PENDING:
                job.message = job.message or "任务取消中"

            future = job.future

        if future and future.cancel():
            with self._lock:
                current = self._jobs.get(job_id)
                if current:
                    current.status = JobStatus.CANCELLED
                    current.progress = min(current.progress, 100)
                    current.message = current.message or "任务已取消"
                    current.finished_at = time.time()
            return True

        return True

    def remove_job(self, job_id: str) -> bool:
        """Remove a job from the manager."""
        with self._lock:
            return self._jobs.pop(job_id, None) is not None

    def update_job(
        self,
        job_id: str,
        *,
        progress: Optional[int] = None,
        message: Optional[str] = None,
        result: Any = None,
        error: Optional[str] = None,
        meta: Optional[Dict[str, Any]] = None,
        status: Optional[JobStatus] = None,
    ) -> bool:
        """Thread-safe partial job update."""
        with self._lock:
            job = self._jobs.get(job_id)
            if not job:
                return False

            if progress is not None:
                job.progress = max(0, min(100, int(progress)))
            if message is not None:
                job.message = message
            if result is not None:
                job.result = result
            if error is not None:
                job.error = error
            if meta:
                job.meta.update(meta)
            if status is not None:
                job.status = status
            return True

    def fail_job(
        self, job_id: str, *, message: str, error: Optional[str] = None
    ) -> bool:
        """Convenience helper to mark a job failed."""
        with self._lock:
            job = self._jobs.get(job_id)
            if not job:
                return False

            job.status = JobStatus.ERROR
            job.message = message
            job.error = error
            job.finished_at = time.time()
            if job.progress < 100:
                job.progress = max(job.progress, 0)
            return True

    def shutdown(self, wait: bool = False) -> None:
        """Shut down worker threads."""
        self._executor.shutdown(wait=wait)

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------
    def _run_job(
        self,
        job_id: str,
        func: Callable[..., Any],
        args: tuple[Any, ...],
        kwargs: Dict[str, Any],
    ) -> None:
        ctx = JobContext(job_id=job_id, _manager=self)

        with self._lock:
            job = self._jobs.get(job_id)
            if not job:
                return
            if job.cancel_requested:
                job.status = JobStatus.CANCELLED
                job.message = job.message or "任务已取消"
                job.finished_at = time.time()
                return

            job.status = JobStatus.RUNNING
            job.started_at = time.time()
            if not job.message:
                job.message = "任务执行中"

        try:
            result = func(ctx, *args, **kwargs)

            with self._lock:
                job = self._jobs.get(job_id)
                if not job:
                    return

                if job.cancel_requested:
                    job.status = JobStatus.CANCELLED
                    job.message = job.message or "任务已取消"
                    job.finished_at = time.time()
                    return

                job.status = JobStatus.SUCCESS
                job.result = result
                job.progress = max(job.progress, 100)
                job.message = job.message or "任务完成"
                job.finished_at = time.time()

        except Exception as exc:
            tb = traceback.format_exc()
            logger.exception("Async job failed: %s (%s)", job_id, exc)
            with self._lock:
                job = self._jobs.get(job_id)
                if not job:
                    return
                job.status = JobStatus.ERROR
                job.error = str(exc)
                job.meta.setdefault("traceback", tb)
                job.message = job.message or "任务执行失败"
                job.finished_at = time.time()

    def _prune_if_needed_unlocked(self) -> None:
        """Prune old finished jobs when capacity is exceeded.

        Keep active jobs preferentially, then newest finished jobs.
        """
        if len(self._jobs) < self._max_jobs:
            return

        active = []
        finished = []
        for job in self._jobs.values():
            if job.status in (JobStatus.PENDING, JobStatus.RUNNING):
                active.append(job)
            else:
                finished.append(job)

        finished.sort(key=lambda j: j.finished_at or j.created_at)

        removable_count = max(0, len(self._jobs) - self._max_jobs + 1)
        for job in finished[:removable_count]:
            self._jobs.pop(job.job_id, None)
