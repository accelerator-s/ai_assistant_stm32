/**
 * TTSConfig.js — TTS 配置
 */

const { ref } = Vue;
const { ElMessage } = ElementPlus;

export default {

  name: 'TTSConfig',

  props: ['config', 'icons'],

  template: `

    <div>

      <div class="az-page-title">
        <span
          class="az-page-title__icon"
          v-html="icons.openai"
        ></span>

        TTS 语音合成
      </div>

      <div class="az-card">

        <div class="az-card__header">
          <span class="az-card__title">
            TTS 配置
          </span>
        </div>

        <div class="az-card__body">

          <el-form
            :model="config.tts"
            label-width="140px"
            class="az-form"
          >

            <el-form-item label="TTS 服务">
              <el-select
                v-model="config.tts.provider"
                style="width:260px;"
              >
                <el-option
                  value="openai_tts"
                  label="OpenAI TTS（兼容接口）"
                />

                <el-option
                  value="custom"
                  label="自定义 API"
                />
              </el-select>
            </el-form-item>

            <el-form-item label="Base URL">
              <el-input
                v-model="config.tts.base_url"
                placeholder="https://api.openai.com/v1"
              ></el-input>
            </el-form-item>

            <el-form-item label="API Key">
              <el-input
                v-model="config.tts.api_key"
                type="password"
                show-password
              ></el-input>
            </el-form-item>

            <el-form-item label="模型">
              <el-input
                v-model="config.tts.model"
                placeholder="tts-1"
              ></el-input>
            </el-form-item>

            <el-form-item label="语音">
              <el-select
                v-model="config.tts.voice"
                style="width:200px;"
              >
                <el-option value="alloy" label="Alloy" />
                <el-option value="echo" label="Echo" />
                <el-option value="fable" label="Fable" />
                <el-option value="onyx" label="Onyx" />
                <el-option value="nova" label="Nova" />
                <el-option value="shimmer" label="Shimmer" />
              </el-select>
            </el-form-item>

            <el-divider content-position="left">
              TTS 测试
            </el-divider>

            <el-form-item label="测试文本">
              <el-input
                v-model="ttsTestText"
                placeholder="请输入测试内容"
              ></el-input>
            </el-form-item>

            <el-form-item>

              <el-button
                type="success"
                @click="handleTTSTest"
                :loading="ttsTesting"
              >
                测试播放
              </el-button>

            </el-form-item>

          </el-form>

        </div>

      </div>

    </div>

  `,

  setup() {

    const ttsTesting = ref(false);

    const ttsTestText = ref(
      '你好，这是 STM32 语音助手测试'
    );

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

      ttsTesting,
      ttsTestText,
      handleTTSTest

    };

  }

};