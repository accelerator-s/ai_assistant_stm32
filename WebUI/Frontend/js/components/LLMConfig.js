/**
 * LLMConfig.js — 大模型 API 配置
 */
import api from '/static/js/api.js';

const { ref } = Vue;
const { ElMessage } = ElementPlus;

export default {
  name: 'LLMConfig',
  props: ['config', 'icons'],

  template: `
    <div>

      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.openai"></span>
        大模型配置
      </div>

      <!-- OpenAI 配置 -->
      <div class="az-card">

        <div class="az-card__header">
          <span class="az-card__title">
            OpenAI 兼容接口设置
          </span>
        </div>

        <div class="az-card__body">

          <el-form
            :model="config.llm"
            label-width="140px"
            class="az-form"
          >

            <el-form-item label="Base URL">
              <el-input
                v-model="config.llm.base_url"
                placeholder="https://api.openai.com/v1"
              ></el-input>

              <span class="az-helper">
                OpenAI 兼容的 API 地址
              </span>
            </el-form-item>

            <el-form-item label="API Key">
              <el-input
                v-model="config.llm.api_key"
                type="password"
                show-password
              ></el-input>
            </el-form-item>

            <el-form-item label="模型">
              <el-input
                v-model="config.llm.model"
                placeholder="gpt-4"
              ></el-input>
            </el-form-item>

            <el-form-item label="System Prompt">
              <el-input
                v-model="config.llm.system_prompt"
                type="textarea"
                :rows="4"
                placeholder="系统提示词..."
              ></el-input>
            </el-form-item>

            <el-form-item label="最大 Token">
              <el-input-number
                v-model="config.llm.max_tokens"
                :min="64"
                :max="4096"
                :step="64"
              />
            </el-form-item>

            <el-form-item label="Temperature">
              <el-slider
                v-model="config.llm.temperature"
                :min="0"
                :max="2"
                :step="0.1"
                style="width:300px;"
                show-input
              />
            </el-form-item>

          </el-form>

       <!-- LLM 连接测试 -->
<div class="az-card">

  <div class="az-card__header">
    <span class="az-card__title">
      连接测试
    </span>
  </div>

  <div class="az-card__body">

    <p
      style="
        font-size:13px;
        color:var(--color-text-secondary);
        margin-bottom:16px;
      "
    >
      向大模型发送一条测试消息，
      验证 API 配置是否正确。
    </p>

    <el-button
      type="primary"
      @click="handleTest"
      :loading="testing"
    >
      测试大模型连接
    </el-button>

    <div
      v-if="testReply"
      style="
        margin-top:12px;
        padding:12px;
        background:var(--color-bg-alt);
        border:1px solid var(--color-border);
        border-radius:var(--radius-sm);
        font-size:13px;
      "
    >
      <strong>模型回复:</strong>
      {{ testReply }}
    </div>

      </div>
      </div>
    </div>
  `,

  setup(props) {

    // LLM 测试
    const testing = ref(false);
    const testReply = ref('');

    // TTS 测试
    const ttsTesting = ref(false);

    const ttsTestText = ref(
      '你好，这是 STM32 语音助手测试'
    );

    // 测试 LLM
    const handleTest = async () => {

      testing.value = true;
      testReply.value = '';

      try {

        const start = Date.now();

        const d = await api.testLLM({
          base_url: props.config.llm.base_url,
          api_key: props.config.llm.api_key,
          model: props.config.llm.model,
        });

        const ms = Date.now() - start;

        if (d.success) {

          ElMessage.success(
            d.message + ` (${ms}ms)`
          );

          testReply.value = d.reply || '';

        } else {

          ElMessage.error(d.message);

        }

      } catch (e) {

        ElMessage.error('测试失败');

      } finally {

        testing.value = false;

      }

    };

    // 测试 TTS
    const handleTTSTest = async () => {

      ttsTesting.value = true;

      try {

        console.log(
          'TTS测试:',
          ttsTestText.value
        );

        ElMessage.success(
          'TTS测试按钮已触发'
        );

      } catch (e) {

        ElMessage.error('TTS测试失败');

      } finally {

        ttsTesting.value = false;

      }

    };

    return {

      testing,
      testReply,
      handleTest,

      ttsTesting,
      ttsTestText,
      handleTTSTest

    };

  }

};