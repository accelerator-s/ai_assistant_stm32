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
                  {{ item.id === 'ode_to_joy' ? '播放欢乐颂' : '开始测试' }}
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

            <el-card shadow="hover" class="step-card"
              :class="{
                'step-success': wavState.status === 'success',
                'step-error': wavState.status === 'error'
              }"
            >
              <div class="step-header">
                <div class="step-icon">WAV</div>
                <div class="step-title">WAV 文件播放</div>
                <el-button
                  type="primary"
                  size="small"
                  :loading="wavState.loading"
                  :disabled="!wavFile"
                  @click="playWav"
                >上传并播放</el-button>
              </div>
              <div class="step-desc">选择一个 WAV 文件，上传到服务器并通过扬声器播放。</div>
              <div style="margin: 10px 0;">
                <input type="file" ref="wavFileInput" accept=".wav" @change="onWavFileChange">
              </div>
              <div v-if="wavFileName" class="step-desc" style="margin-bottom: 6px;">
                已选择文件：{{ wavFileName }}
              </div>

              <el-progress
                v-if="wavState.loading || wavState.progress > 0"
                :percentage="wavState.progress || 0"
                :status="progressStatus(wavState.status, wavState.loading)"
                :stroke-width="8"
                style="margin: 10px 0 6px 0"
              />

              <div v-if="wavState.hint" class="step-desc" style="margin-bottom: 4px;">
                {{ wavState.hint }}
              </div>

              <div v-if="wavState.result" class="step-result" :class="'text-' + wavState.status">
                {{ wavState.result }}
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

    const wavFile = ref(null);
    const wavFileName = ref('');
    const wavState = ref(createState());

    const onWavFileChange = (e) => {
      const file = e.target.files?.[0] || null;
      wavFile.value = file;
      wavFileName.value = file ? file.name : '';
    };

    const playWav = async () => {
      if (!wavFile.value) return;
      const caseId = 'wav_play';
      const token = Date.now();

      wavState.value = {
        ...createState(),
        loading: true,
        pollToken: token,
      };
      // 注册到 states 中以便 pollJob 可以读取 pollToken
      states.value[caseId] = wavState.value;

      try {
        const formData = new FormData();
        formData.append('file', wavFile.value);

        const submitRes = await api.playWavFile(formData);
        const jobId = submitRes?.job_id;
        if (!submitRes?.success || !jobId) {
          throw new Error(submitRes?.context || submitRes?.message || '上传失败');
        }

        const patch1 = {
          progress: 5,
          hint: submitRes.message || '文件已上传，正在播放...',
          jobId,
        };
        wavState.value = { ...wavState.value, ...patch1 };
        states.value[caseId] = wavState.value;

        const job = await pollJob(caseId, jobId, token, 60000);
        wavState.value = {
          ...wavState.value,
          loading: false,
          status: 'success',
          progress: 100,
          result: job?.result?.context || 'WAV 文件播放完成',
        };
      } catch (err) {
        wavState.value = {
          ...wavState.value,
          loading: false,
          status: 'error',
          result: err?.message || '播放失败，请重试',
        };
      }
      states.value[caseId] = wavState.value;
    };

    onMounted(loadCases);
    onBeforeUnmount(() => {
      disposed.value = true;
    });

    return {
      testCases,
      states,
      loadingCases,
      loadCases,
      runCase,
      progressStatus,
      wavFile,
      wavFileName,
      wavState,
      onWavFileChange,
      playWav,
    };
  },
};
