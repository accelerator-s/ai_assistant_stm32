import api from "../api.js";

const { ref, onMounted, onBeforeUnmount } = Vue;

export default {
  name: "SpeakerTest",
  props: ["status", "icons"],
  template: `
    <div>
      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.speaker"></span>
        音响测试
      </div>

      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">扬声器与功放测试集</span>
          <el-button size="small" @click="loadCases" :loading="loadingCases">刷新测试集</el-button>
        </div>
        <div class="az-card__body">
          <el-alert
            title="请先调低外接音响音量"
            type="warning"
            description="测试会播放单音、扫频和不同音量的提示音。每个测试会异步执行，页面通过任务状态轮询结果。"
            show-icon
            :closable="false"
            style="margin-bottom: 16px"
          />

          <el-card
            shadow="hover"
            class="step-card"
            :class="{
              'step-success': localState.status === 'success',
              'step-error': localState.status === 'error'
            }"
            style="margin-bottom: 16px"
          >
            <div class="step-header">
              <div class="step-icon">♪</div>
              <div class="step-title">播放本地 WAV</div>
              <el-button
                type="primary"
                size="small"
                :disabled="!selectedFile || localState.loading"
                :loading="localState.loading"
                @click="playLocalAudio"
              >
                开始播放
              </el-button>
            </div>
            <div style="display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-top:10px;">
              <el-upload
                :auto-upload="false"
                :limit="1"
                accept=".wav,audio/wav"
                :on-change="handleLocalFileChange"
                :on-remove="handleLocalFileRemove"
              >
                <el-button size="small">选择 WAV</el-button>
              </el-upload>
              <span class="step-desc" v-if="selectedFile">{{ selectedFile.name }}</span>
            </div>

            <el-progress
              v-if="localState.loading || localState.progress > 0"
              :percentage="localState.progress || 0"
              :status="progressStatus(localState.status, localState.loading)"
              :stroke-width="8"
              style="margin: 10px 0 6px 0"
            />
            <div v-if="localState.hint" class="step-desc" style="margin-bottom: 4px;">
              {{ localState.hint }}
            </div>
            <div v-if="localState.result" class="step-result" :class="'text-' + localState.status">
              {{ localState.result }}
            </div>
          </el-card>

          <div class="test-steps">
            <el-card
              v-for="(item, index) in testCases"
              :key="item.id"
              shadow="hover"
              class="step-card"
              :class="{
                'step-success': states[item.id]?.status === 'success',
                'step-error': states[item.id]?.status === 'error'
              }"
            >
              <div class="step-header">
                <div class="step-icon">{{ index + 1 }}</div>
                <div class="step-title">{{ item.name }}</div>
                <el-button
                  type="primary"
                  size="small"
                  :loading="states[item.id]?.loading"
                  @click="runCase(item)"
                >
                  开始测试
                </el-button>
              </div>
              <div class="step-desc">{{ item.description }}</div>
              <div class="step-desc">命令：{{ item.command }}，期望响应：{{ item.expected }}</div>

              <el-progress
                v-if="states[item.id]?.loading || states[item.id]?.progress > 0"
                :percentage="states[item.id]?.progress || 0"
                :status="progressStatus(states[item.id]?.status, states[item.id]?.loading)"
                :stroke-width="8"
                style="margin: 10px 0 6px 0"
              />

              <div v-if="states[item.id]?.hint" class="step-desc" style="margin-bottom: 4px;">
                {{ states[item.id].hint }}
              </div>

              <div v-if="states[item.id]?.result" class="step-result" :class="'text-' + states[item.id].status">
                {{ states[item.id].result }}
              </div>
            </el-card>
          </div>
        </div>
      </div>
    </div>
  `,
  setup() {
    const testCases = ref([]);
    const states = ref({});
    const selectedFile = ref(null);
    const localState = ref({
      loading: false,
      status: "pending",
      progress: 0,
      hint: "",
      result: "",
      jobId: "",
      pollToken: 0,
    });
    const loadingCases = ref(false);
    const disposed = ref(false);

    const createState = () => ({
      loading: false,
      status: "pending",
      progress: 0,
      hint: "",
      result: "",
      jobId: "",
      pollToken: 0,
    });

    const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

    const ensureState = (caseId) => {
      if (!states.value[caseId]) {
        states.value[caseId] = createState();
      }
      return states.value[caseId];
    };

    const setCaseState = (caseId, patch) => {
      states.value[caseId] = { ...ensureState(caseId), ...patch };
    };

    const setLocalState = (patch) => {
      localState.value = { ...localState.value, ...patch };
    };

    const progressStatus = (status, loading) => {
      if (loading) return undefined;
      if (status === "success") return "success";
      if (status === "error") return "exception";
      return undefined;
    };

    const loadCases = async () => {
      loadingCases.value = true;
      try {
        const resp = await api.getSpeakerTestCases();
        testCases.value = resp?.cases || [];
        testCases.value.forEach((item) => ensureState(item.id));
      } finally {
        loadingCases.value = false;
      }
    };

    const pollJob = async (caseId, jobId, token, timeout) => {
      const started = Date.now();
      while (!disposed.value && states.value[caseId]?.pollToken === token) {
        const resp = await api.getSpeakerTestJob(jobId);
        const job = resp?.job;
        if (!job) throw new Error("任务不存在");

        setCaseState(caseId, {
          progress: typeof job.progress === "number" ? job.progress : 0,
          hint: job.message || "",
          jobId: job.job_id || jobId,
        });

        if (job.status === "success") return job;
        if (job.status === "error") {
          throw new Error(job.error || job.message || "音响测试失败");
        }
        if (job.status === "cancelled") throw new Error("任务已取消");

        if (Date.now() - started > timeout) {
          try {
            await api.cancelSpeakerTestJob(jobId);
          } catch (_) {}
          throw new Error("任务执行超时");
        }

        await sleep(500);
      }
      throw new Error("任务已终止");
    };

    const runCase = async (item) => {
      const token = Date.now();
      setCaseState(item.id, {
        loading: true,
        status: "pending",
        progress: 0,
        hint: "",
        result: "",
        jobId: "",
        pollToken: token,
      });

      try {
        const submitRes = await api.runSpeakerTestCase(item.id);
        const jobId = submitRes?.job_id;
        if (!submitRes?.success || !jobId) {
          throw new Error(submitRes?.context || submitRes?.message || "任务提交失败");
        }

        setCaseState(item.id, {
          progress: 5,
          hint: submitRes.message || "任务已提交，正在等待设备执行...",
          jobId,
        });

        const job = await pollJob(item.id, jobId, token, (item.timeout || 8) * 1000 + 5000);
        setCaseState(item.id, {
          loading: false,
          status: "success",
          progress: 100,
          result: job?.result?.context || `${item.name}通过`,
        });
      } catch (err) {
        setCaseState(item.id, {
          loading: false,
          status: "error",
          result: err?.message || "请求失败，请重试",
        });
      }
    };

    const handleLocalFileChange = (file) => {
      selectedFile.value = file?.raw || null;
      setLocalState({
        status: "pending",
        progress: 0,
        hint: "",
        result: "",
        jobId: "",
      });
    };

    const handleLocalFileRemove = () => {
      selectedFile.value = null;
    };

    const playLocalAudio = async () => {
      if (!selectedFile.value) return;

      const token = Date.now();
      setLocalState({
        loading: true,
        status: "pending",
        progress: 0,
        hint: "",
        result: "",
        jobId: "",
        pollToken: token,
      });

      try {
        const submitRes = await api.playLocalSpeakerAudio(selectedFile.value);
        const jobId = submitRes?.job_id;
        if (!submitRes?.success || !jobId) {
          throw new Error(submitRes?.context || submitRes?.message || "任务提交失败");
        }

        setLocalState({
          progress: 5,
          hint: submitRes.message || "任务已提交，正在准备播放...",
          jobId,
        });

        const started = Date.now();
        while (!disposed.value && localState.value.pollToken === token) {
          const resp = await api.getSpeakerTestJob(jobId);
          const job = resp?.job;
          if (!job) throw new Error("任务不存在");

          setLocalState({
            progress: typeof job.progress === "number" ? job.progress : 0,
            hint: job.message || "",
            jobId: job.job_id || jobId,
          });

          if (job.status === "success") {
            setLocalState({
              loading: false,
              status: "success",
              progress: 100,
              result: job?.result?.context || "本地音频播放完成",
            });
            return;
          }
          if (job.status === "error") {
            throw new Error(job.error || job.message || "本地音频播放失败");
          }
          if (job.status === "cancelled") throw new Error("任务已取消");
          if (Date.now() - started > 120000) {
            try {
              await api.cancelSpeakerTestJob(jobId);
            } catch (_) {}
            throw new Error("任务执行超时");
          }

          await sleep(500);
        }
      } catch (err) {
        setLocalState({
          loading: false,
          status: "error",
          result: err?.message || "请求失败，请重试",
        });
      }
    };

    onMounted(loadCases);
    onBeforeUnmount(() => {
      disposed.value = true;
    });

    return {
      testCases,
      states,
      selectedFile,
      localState,
      loadingCases,
      loadCases,
      runCase,
      handleLocalFileChange,
      handleLocalFileRemove,
      playLocalAudio,
      progressStatus,
    };
  },
};
