/**
 * AdvancedSettings.js — 高级系统设置
 */
export default {
  name: 'AdvancedSettings',
  props: ['config', 'icons'],
  template: `
    <div>
      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.advanced"></span>
        高级设置
      </div>

      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">服务配置</span>
        </div>
        <div class="az-card__body">
          <el-form :model="config.advanced" label-width="160px" class="az-form">
            <el-form-item label="WebUI 端口">
              <el-input-number v-model="config.advanced.service_port" :min="1024" :max="65535" />
              <span class="az-helper">修改后需重启服务生效</span>
            </el-form-item>
            <el-form-item label="日志级别">
              <el-select v-model="config.advanced.log_level" style="width:200px;">
                <el-option value="DEBUG" label="DEBUG" />
                <el-option value="INFO" label="INFO" />
                <el-option value="WARNING" label="WARNING" />
                <el-option value="ERROR" label="ERROR" />
              </el-select>
            </el-form-item>
            <el-form-item label="单会话最大历史">
              <el-input-number v-model="config.advanced.max_history_per_session" :min="10" :max="500" :step="10" />
              <span class="az-helper">每个会话保留的最大消息数</span>
            </el-form-item>
            <el-form-item label="音频缓冲超时">
              <el-input-number v-model="config.advanced.audio_buffer_timeout_sec" :min="5" :max="120" />
              <span class="az-helper">秒，超时后自动清空缓冲</span>
            </el-form-item>
          </el-form>
        </div>
      </div>
    </div>
  `
};
