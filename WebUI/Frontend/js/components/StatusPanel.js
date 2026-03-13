/**
 * StatusPanel.js — 概览仪表板
 */
import api from '/static/js/api.js';

const { ref, onMounted, onUnmounted } = Vue;

export default {
  name: 'StatusPanel',
  props: ['config', 'status', 'icons'],
  emits: ['refresh'],
  template: `
    <div>
      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.dashboard"></span>
        系统概览
      </div>

      <!-- 连接状态横幅 -->
      <div class="az-status-banner" :class="status.device_connected ? 'az-status-banner--ok' : 'az-status-banner--err'">
        <div class="az-status-banner__icon" v-html="status.device_connected ? icons.checkCircle : icons.errorCircle"></div>
        <div class="az-status-banner__text">
          <div class="az-status-banner__title">{{ status.device_connected ? '设备已连接' : '设备未连接' }}</div>
          <div class="az-status-banner__desc">{{ status.device_connected ? status.device_info : '等待 ESP8266 终端连接...' }}</div>
        </div>
        <el-button size="small" @click="$emit('refresh')">刷新状态</el-button>
      </div>

      <!-- 指标卡片 -->
      <div class="az-metrics">
        <div class="az-metric">
          <div class="az-metric__label">设备名称</div>
          <div class="az-metric__value" style="font-size:18px;">{{ status.device_name || 'STM32F103VET6' }}</div>
        </div>
        <div class="az-metric">
          <div class="az-metric__label">TCP 端口</div>
          <div class="az-metric__value">{{ status.tcp_port || '-' }}</div>
        </div>
        <div class="az-metric">
          <div class="az-metric__label">在线设备</div>
          <div class="az-metric__value" :class="status.connected_clients > 0 ? 'az-metric__value--success' : 'az-metric__value--danger'">
            {{ status.connected_clients || 0 }}
          </div>
        </div>
        <div class="az-metric">
          <div class="az-metric__label">大模型</div>
          <div class="az-metric__value" style="font-size:16px;">{{ config.llm?.model || '未配置' }}</div>
        </div>
      </div>

      <!-- 快速操作 -->
      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">快速操作</span>
        </div>
        <div class="az-card__body" style="display:flex;gap:12px;flex-wrap:wrap;">
          <el-button type="primary" @click="testDevice" :loading="testing.device">测试设备连接</el-button>
          <el-button @click="testSpeech" :loading="testing.speech">测试语音识别</el-button>
          <el-button @click="testLLM" :loading="testing.llm">测试大模型</el-button>
        </div>
      </div>

      <!-- 系统信息 -->
      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">配置摘要</span>
        </div>
        <div class="az-card__body">
          <el-descriptions :column="2" border size="small">
            <el-descriptions-item label="语音识别">{{ config.speech?.provider || '未配置' }}</el-descriptions-item>
            <el-descriptions-item label="语音模型">{{ config.speech?.model || '-' }}</el-descriptions-item>
            <el-descriptions-item label="LLM 地址">{{ config.llm?.base_url || '未配置' }}</el-descriptions-item>
            <el-descriptions-item label="LLM 模型">{{ config.llm?.model || '-' }}</el-descriptions-item>
            <el-descriptions-item label="TTS 服务">{{ config.tts?.provider || '未配置' }}</el-descriptions-item>
            <el-descriptions-item label="TTS 语音">{{ config.tts?.voice || '-' }}</el-descriptions-item>
          </el-descriptions>
        </div>
      </div>
    </div>
  `,

  setup(props) {
    const { ElMessage } = ElementPlus;
    const testing = ref({ device: false, speech: false, llm: false });

    const testDevice = async () => {
      testing.value.device = true;
      try {
        const d = await api.testDevice();
        d.success ? ElMessage.success(d.message) : ElMessage.error(d.message);
      } catch (e) {
        ElMessage.error('测试失败');
      } finally { testing.value.device = false; }
    };

    const testSpeech = async () => {
      testing.value.speech = true;
      try {
        const d = await api.testSpeech({});
        d.success ? ElMessage.success(d.message) : ElMessage.error(d.message);
      } catch (e) {
        ElMessage.error('测试失败');
      } finally { testing.value.speech = false; }
    };

    const testLLM = async () => {
      testing.value.llm = true;
      try {
        const d = await api.testLLM({});
        d.success ? ElMessage.success(d.message) : ElMessage.error(d.message);
      } catch (e) {
        ElMessage.error('测试失败');
      } finally { testing.value.llm = false; }
    };

    return { testing, testDevice, testSpeech, testLLM };
  }
};
