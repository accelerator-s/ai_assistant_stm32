/**
 * TTSConfig.js — TTS 配置（Azure TTS）
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
            Azure TTS 配置
          </span>
        </div>

        <div class="az-card__body">

          <el-form
            :model="config.tts"
            label-width="140px"
            class="az-form"
          >

            <el-form-item label="区域 (Region)">
              <el-input
                v-model="config.tts.azure_region"
                placeholder="japanwest"
              ></el-input>
            </el-form-item>

            <el-form-item label="订阅密钥">
              <el-input
                v-model="config.tts.azure_key"
                type="password"
                show-password
                placeholder="Azure Speech Service Key"
              ></el-input>
            </el-form-item>

            <el-form-item label="语音角色">
              <el-select
                v-model="config.tts.azure_voice"
                style="width:300px;"
                filterable
              >
                <el-option-group label="女声">
                  <el-option value="zh-CN-XiaoxiaoNeural" label="晓晓 (Xiaoxiao) - 温暖亲切" />
                  <el-option value="zh-CN-XiaoyiNeural" label="晓伊 (Xiaoyi) - 活泼" />
                  <el-option value="zh-CN-XiaochenNeural" label="晓辰 (Xiaochen) - 沉稳" />
                  <el-option value="zh-CN-XiaohanNeural" label="晓涵 (Xiaohan) - 温和" />
                  <el-option value="zh-CN-XiaomengNeural" label="晓梦 (Xiaomeng) - 甜美" />
                  <el-option value="zh-CN-XiaomoNeural" label="晓墨 (Xiaomo) - 知性" />
                  <el-option value="zh-CN-XiaoruiNeural" label="晓睿 (Xiaorui) - 成熟" />
                  <el-option value="zh-CN-XiaoshuangNeural" label="晓双 (Xiaoshuang) - 童声" />
                  <el-option value="zh-CN-XiaoyanNeural" label="晓颜 (Xiaoyan) - 明朗" />
                  <el-option value="zh-CN-XiaozhenNeural" label="晓甄 (Xiaozhen) - 优雅" />
                </el-option-group>
                <el-option-group label="男声">
                  <el-option value="zh-CN-YunxiNeural" label="云希 (Yunxi) - 阳光少年" />
                  <el-option value="zh-CN-YunjianNeural" label="云健 (Yunjian) - 沉稳" />
                  <el-option value="zh-CN-YunyangNeural" label="云扬 (Yunyang) - 新闻播报" />
                  <el-option value="zh-CN-YunfengNeural" label="云枫 (Yunfeng) - 磁性" />
                  <el-option value="zh-CN-YunhaoNeural" label="云皓 (Yunhao) - 自然" />
                  <el-option value="zh-CN-YunxiaNeural" label="云夏 (Yunxia) - 少年" />
                  <el-option value="zh-CN-YunzeNeural" label="云泽 (Yunze) - 成熟" />
                </el-option-group>
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
            使用当前配置测试 Azure TTS 语音合成服务，合成成功后可在线播放。
          </p>

          <el-form label-width="140px" class="az-form">
            <el-form-item label="测试文本">
              <el-input
                v-model="ttsTestText"
                placeholder="请输入测试内容"
              ></el-input>
            </el-form-item>

            <el-form-item>
              <el-button
                type="primary"
                @click="handleTTSTest"
                :loading="ttsTesting"
              >
                测试播放
              </el-button>
            </el-form-item>

            <el-form-item v-if="audioUrl">
              <audio
                :src="audioUrl"
                controls
                style="width: 100%;"
              ></audio>
            </el-form-item>
          </el-form>

        </div>
      </div>

    </div>

  `,

  setup(props) {

    const ttsTesting = ref(false);

    const ttsTestText = ref(
      '你好，这是 STM32 语音助手测试'
    );

    const audioUrl = ref('');

    const handleTTSTest = async () => {

      ttsTesting.value = true;
      audioUrl.value = '';

      try {
        const ttsConfig = props.config.tts;
        const params = {
          text: ttsTestText.value,
          azure_region: ttsConfig.azure_region,
          azure_key: ttsConfig.azure_key,
          azure_voice: ttsConfig.azure_voice || 'zh-CN-XiaoxiaoNeural',
        };

        const { default: api } = await import('../api.js');
        const result = await api.testTTS(params);

        if (result.success && result.audio_url) {
          audioUrl.value = result.audio_url;
          ElMessage.success('语音合成成功');
        } else {
          ElMessage.error(result.message || 'TTS 测试失败');
        }

      } catch (e) {
        const msg = e.response?.data?.message || e.message || 'TTS 测试失败';
        ElMessage.error(msg);

      } finally {
        ttsTesting.value = false;
      }

    };

    return {

      ttsTesting,
      ttsTestText,
      audioUrl,
      handleTTSTest

    };

  }

};
