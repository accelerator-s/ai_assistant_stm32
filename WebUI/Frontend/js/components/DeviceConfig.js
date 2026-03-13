/**
 * DeviceConfig.js — 设备通信配置
 */
import api from '/static/js/api.js';

const { ref } = Vue;
const { ElMessage } = ElementPlus;

export default {
  name: 'DeviceConfig',
  props: ['config', 'icons'],
  template: `
    <div>
      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.device"></span>
        设备通信
      </div>

      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">TCP 通信设置</span>
        </div>
        <div class="az-card__body">
          <el-form :model="config.device" label-width="140px" class="az-form">
            <el-form-item label="监听地址">
              <el-input v-model="config.device.tcp_host" placeholder="0.0.0.0"></el-input>
              <span class="az-helper">ESP8266 连接的服务端地址</span>
            </el-form-item>
            <el-form-item label="TCP 端口">
              <el-input-number v-model="config.device.tcp_port" :min="1024" :max="65535" />
              <span class="az-helper">ESP8266 连接的端口号</span>
            </el-form-item>
            <el-form-item label="设备名称">
              <el-input v-model="config.device.device_name" placeholder="STM32F103VET6"></el-input>
            </el-form-item>
            <el-form-item label="采样率">
              <el-select v-model="config.device.audio_sample_rate" style="width:200px;">
                <el-option :value="8000" label="8000 Hz" />
                <el-option :value="16000" label="16000 Hz（推荐）" />
                <el-option :value="44100" label="44100 Hz" />
              </el-select>
              <span class="az-helper">I2S 音频采样率</span>
            </el-form-item>
            <el-form-item label="位深度">
              <el-select v-model="config.device.audio_bit_depth" style="width:200px;">
                <el-option :value="16" label="16 bit" />
                <el-option :value="24" label="24 bit" />
                <el-option :value="32" label="32 bit" />
              </el-select>
            </el-form-item>
          </el-form>
        </div>
      </div>

      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">连接测试</span>
        </div>
        <div class="az-card__body">
          <p style="font-size:13px;color:var(--color-text-secondary);margin-bottom:16px;">
            点击下方按钮测试 TCP 服务器状态及设备连接情况。
          </p>
          <el-button type="primary" @click="handleTest" :loading="testing">测试设备连接</el-button>
        </div>
      </div>
    </div>
  `,

  setup(props) {
    const testing = ref(false);

    const handleTest = async () => {
      testing.value = true;
      try {
        const start = Date.now();
        const d = await api.testDevice();
        const ms = Date.now() - start;
        d.success
          ? ElMessage.success(d.message + ` (${ms}ms)`)
          : ElMessage.error(d.message);
      } catch (e) {
        ElMessage.error('连接测试失败');
      } finally { testing.value = false; }
    };

    return { testing, handleTest };
  }
};
