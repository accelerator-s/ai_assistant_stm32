/**
 * SpeechConfig.js — 语音识别服务配置
 */
import api from '/static/js/api.js';

const { ref } = Vue;
const { ElMessage } = ElementPlus;

export default {
  name: 'SpeechConfig',
  props: ['config', 'icons'],
  template: `
    <div>
      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.mic"></span>
        语音识别
      </div>

      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">语音识别服务设置</span>
        </div>
        <div class="az-card__body">
          <el-form :model="config.speech" label-width="140px" class="az-form">
            <el-form-item label="服务类型">
              <el-select v-model="config.speech.provider" style="width:260px;">
                <el-option value="tencent" label="腾讯云 (Tencent Cloud Asynchronous ASR)" />
                <el-option value="openai_whisper" label="OpenAI Whisper（兼容接口）" />
                <el-option value="custom" label="自定义 API" />
              </el-select>
            </el-form-item>
            <template v-if="config.speech.provider === 'tencent'">
              <el-form-item label="SecretId">
                <el-input v-model="config.speech.secret_id" placeholder="Tencent Cloud SecretId"></el-input>
                <span class="az-helper">腾讯云访问秘钥 SecretId</span>
              </el-form-item>
              <el-form-item label="SecretKey">
                <el-input v-model="config.speech.secret_key" type="password" show-password placeholder="Tencent Cloud SecretKey"></el-input>
                <span class="az-helper">腾讯云访问秘钥 SecretKey</span>
              </el-form-item>
              <el-form-item label="Region">
                <el-input v-model="config.speech.region" placeholder="ap-shanghai"></el-input>
                <span class="az-helper">如 ap-shanghai, ap-beijing, ap-guangzhou 等</span>
              </el-form-item>
              <el-form-item label="引擎模型">
                <el-input v-model="config.speech.model" placeholder="16k_zh"></el-input>
                <span class="az-helper">默认为 16k_zh (中文)</span>
              </el-form-item>
            </template>
            <template v-else>
              <el-form-item label="Base URL">
                <el-input v-model="config.speech.base_url" placeholder="https://api.openai.com/v1"></el-input>
                <span class="az-helper">语音识别服务地址</span>
              </el-form-item>
              <el-form-item label="API Key">
                <el-input v-model="config.speech.api_key" type="password" show-password placeholder="留空则不传"></el-input>
              </el-form-item>
              <el-form-item label="模型">
                <el-input v-model="config.speech.model" placeholder="whisper-1"></el-input>
              </el-form-item>
            </template>
            <el-form-item label="语言">
              <el-select v-model="config.speech.language" style="width:200px;">
                <el-option value="zh" label="中文" />
                <el-option value="en" label="英文" />
                <el-option value="ja" label="日文" />
                <el-option value="auto" label="自动检测" />
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
            使用当前配置测试语音识别服务的可达性。
          </p>
          <el-button type="primary" @click="handleTest" :loading="testing">测试语音识别服务</el-button>
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
        const d = await api.testSpeech({
          provider: props.config.speech.provider,
          base_url: props.config.speech.base_url,
          api_key: props.config.speech.api_key,
          secret_id: props.config.speech.secret_id,
          secret_key: props.config.speech.secret_key,
          region: props.config.speech.region,
          model: props.config.speech.model,
        });
        const ms = Date.now() - start;
        d.success
          ? ElMessage.success(d.message + ` (${ms}ms)`)
          : ElMessage.error(d.message);
      } catch (e) {
        ElMessage.error('测试失败');
      } finally { testing.value = false; }
    };

    return { testing, handleTest };
  }
};
