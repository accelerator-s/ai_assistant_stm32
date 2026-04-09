import api from "../api.js";

const { ref, onBeforeUnmount } = Vue;

export default {
  name: "MicTest",
  props: ["status", "icons"],
  template: `
    <div>
      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.mic"></span>
        麦克风测试
      </div>

      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">INMP441 麦克风连接测试</span>
        </div>
        <div class="az-card__body">
          <el-alert
            title="三步硬件连接测试流程"
            type="info"
            description="所有测试均改为异步任务执行，页面不会因等待设备响应而卡住。"
            show-icon
            :closable="false"
            style="margin-bottom: 16px"
          />

          <div class="test-steps">
            <el-card
              shadow="hover"
              class="step-card"
              :class="{
                'step-success': steps[0].status === 'success',
                'step-error': steps[0].status === 'error'
              }"
            >
              <div class="step-header">
                <div class="step-icon">1</div>
                <div class="step-title">硬件通信检测</div>
                <el-button
                  type="primary"
                  size="small"
                  :loading="steps[0].loading"
                  @click="runStep(0)"
                >
                  开始检测
                </el-button>
              </div>
              <div class="step-desc">验证 STM32 与 INMP441 之间的 I2S 总线通信是否正常。</div>

              <el-progress
                v-if="steps[0].loading || steps[0].progress > 0"
                :percentage="steps[0].progress"
                :status="progressStatus(steps[0].status, steps[0].loading)"
                :stroke-width="8"
                style="margin: 10px 0 6px 0"
              />

              <div v-if="steps[0].hint" class="step-desc" style="margin-bottom: 4px;">
                {{ steps[0].hint }}
              </div>

              <div v-if="steps[0].result" class="step-result" :class="'text-' + steps[0].status">
                {{ steps[0].result }}
              </div>
            </el-card>

            <el-card
              shadow="hover"
              class="step-card"
              :class="{
                'step-success': steps[1].status === 'success',
                'step-error': steps[1].status === 'error'
              }"
            >
              <div class="step-header">
                <div class="step-icon">2</div>
                <div class="step-title">录音采集回放</div>
                <el-button
                  type="primary"
                  size="small"
                  :loading="steps[1].loading"
                  :disabled="steps[0].status !== 'success'"
                  @click="runStep(1)"
                >
                  录制3秒
                </el-button>
              </div>
              <div class="step-desc">命令开发板录制 3 秒音频并通过 TCP 上传，在页面播放以确认音质。</div>

              <el-progress
                v-if="steps[1].loading || steps[1].progress > 0"
                :percentage="steps[1].progress"
                :status="progressStatus(steps[1].status, steps[1].loading)"
                :stroke-width="8"
                style="margin: 10px 0 6px 0"
              />

              <div v-if="steps[1].hint" class="step-desc" style="margin-bottom: 4px;">
                {{ steps[1].hint }}
              </div>

              <div v-if="steps[1].audioUrl" class="step-audio">
                <audio :src="steps[1].audioUrl" controls style="height: 36px; width: 100%;"></audio>
              </div>

              <div v-if="steps[1].result" class="step-result" :class="'text-' + steps[1].status">
                {{ steps[1].result }}
              </div>
            </el-card>

            <el-card
              shadow="hover"
              class="step-card"
              :class="{
                'step-success': steps[2].status === 'success',
                'step-error': steps[2].status === 'error'
              }"
            >
              <div class="step-header">
                <div class="step-icon">3</div>
                <div class="step-title">语音识别测试</div>
                <el-button
                  type="primary"
                  size="small"
                  :loading="steps[2].loading"
                  :disabled="steps[1].status !== 'success'"
                  @click="runStep(2)"
                >
                  识别云端验证
                </el-button>
              </div>
              <div class="step-desc">将刚才采集到的音频发送到云端识别引擎，验证识别率。</div>

              <el-progress
                v-if="steps[2].loading || steps[2].progress > 0"
                :percentage="steps[2].progress"
                :status="progressStatus(steps[2].status, steps[2].loading)"
                :stroke-width="8"
                style="margin: 10px 0 6px 0"
              />

              <div v-if="steps[2].hint" class="step-desc" style="margin-bottom: 4px;">
                {{ steps[2].hint }}
              </div>

              <div v-if="steps[2].result" class="step-result" :class="'text-' + steps[2].status">
                {{ steps[2].result }}
              </div>
            </el-card>
          </div>
        </div>
      </div>
    </div>
  `,
  setup() {
    const STEP_CONFIG = [
      {
        submit: () => api.testMicHardware(),
        success: (job) => ({
          status: "success",
          result: job?.result?.context || "通信正常！成功读取到麦克风数据帧。",
        }),
        error: (job) => ({
          status: "error",
          result:
            "硬件检测失败: " +
            (job?.error || job?.message || "未检测到有效数据"),
        }),
        timeout: 15000,
      },
      {
        submit: () => api.testMicRecord(),
        success: (job) => ({
          status: "success",
          result: job?.result?.context || "录音采集成功，请点击播放按钮试听。",
          audioUrl: job?.result?.audio_url || "",
        }),
        error: (job) => ({
          status: "error",
          result:
            "采集超时或失败: " +
            (job?.error || job?.message || "开发板未返回录音结果"),
        }),
        timeout: 25000,
      },
      {
        submit: () => api.testMicRecognize(),
        success: (job) => ({
          status: "success",
          result:
            '识别结果: "' + (job?.result?.text || "") + '"\n' +
            '大模型回复: "' + (job?.result?.reply || "") + '"',
        }),
        error: (job) => ({
          status: "error",
          result:
            "识别失败: " + (job?.error || job?.message || "识别任务执行失败"),
        }),
        timeout: 45000,
      },
    ];

    const createStep = () => ({
      loading: false,
      status: "pending",
      result: "",
      audioUrl: "",
      progress: 0,
      hint: "",
      jobId: "",
      pollToken: 0,
    });

    const steps = ref([createStep(), createStep(), createStep()]);
    const disposed = ref(false);

    const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

    const resetStepVisual = (index) => {
      steps.value[index].result = "";
      steps.value[index].audioUrl = "";
      steps.value[index].progress = 0;
      steps.value[index].hint = "";
      steps.value[index].jobId = "";
    };

    const setStepState = (index, patch) => {
      steps.value[index] = { ...steps.value[index], ...patch };
    };

    const progressStatus = (status, loading) => {
      if (loading) return undefined;
      if (status === "success") return "success";
      if (status === "error") return "exception";
      return undefined;
    };

    const applyJobProgress = (index, job, token) => {
      if (disposed.value) return;
      const step = steps.value[index];
      if (step.pollToken !== token) return;

      setStepState(index, {
        progress:
          typeof job.progress === "number" ? job.progress : step.progress,
        hint: job.message || step.hint,
        jobId: job.job_id || step.jobId,
      });
    };

    const finishStepSuccess = (index, token, payload) => {
      if (disposed.value) return;
      if (steps.value[index].pollToken !== token) return;
      setStepState(index, {
        loading: false,
        ...payload,
        progress: 100,
      });
    };

    const finishStepError = (index, token, message) => {
      if (disposed.value) return;
      if (steps.value[index].pollToken !== token) return;
      setStepState(index, {
        loading: false,
        status: "error",
        result: message,
        progress:
          steps.value[index].progress > 0 ? steps.value[index].progress : 0,
      });
    };

    const pollJob = async (index, jobId, token, timeout) => {
      const started = Date.now();

      while (!disposed.value && steps.value[index].pollToken === token) {
        const resp = await api.getMicTestJob(jobId);
        const job = resp?.job;

        if (!job) {
          throw new Error("任务不存在");
        }

        applyJobProgress(index, job, token);

        if (job.status === "success") {
          return job;
        }

        if (job.status === "error") {
          throw new Error(job.error || job.message || "任务执行失败");
        }

        if (job.status === "cancelled") {
          throw new Error("任务已取消");
        }

        if (Date.now() - started > timeout) {
          try {
            await api.cancelMicTestJob(jobId);
          } catch (_) {}
          throw new Error("任务执行超时");
        }

        await sleep(500);
      }

      throw new Error("任务已终止");
    };

    const runStep = async (index) => {
      const config = STEP_CONFIG[index];
      const nextToken = Date.now() + index;

      setStepState(index, {
        loading: true,
        status: "pending",
        pollToken: nextToken,
      });
      resetStepVisual(index);

      try {
        const submitRes = await config.submit();
        const jobId = submitRes?.job_id;
        if (!submitRes?.success || !jobId) {
          throw new Error(
            submitRes?.context || submitRes?.message || "任务提交失败",
          );
        }

        setStepState(index, {
          progress: 5,
          hint: submitRes.message || "任务已提交，正在等待设备执行...",
          jobId,
        });

        const job = await pollJob(index, jobId, nextToken, config.timeout);
        finishStepSuccess(index, nextToken, config.success(job));
      } catch (err) {
        finishStepError(
          index,
          nextToken,
          err?.message || config.error()?.result || "请求失败请重试",
        );
      }
    };

    onBeforeUnmount(() => {
      disposed.value = true;
    });

    return {
      steps,
      runStep,
      progressStatus,
    };
  },
};
