const { ref, computed } = Vue;
const { ElMessage } = ElementPlus;

export default {
  name: 'UserSettings',
  props: ['config', 'icons'],
  template: `
    <div>
      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.device"></span>
        用户设置
      </div>

      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">个性化设置</span>
        </div>
        <div class="az-card__body">
          <p class="az-section-tip">此页修改内容会跟随顶部“保存配置”按钮一起生效并同步到设备。</p>

          <el-form :model="userConfig" class="az-form" label-width="140px">
          <el-form-item label="自定义用户名">
            <el-input 
              v-model="userConfig.username" 
              placeholder="请输入用户名 (中英数字, 限10个字符)" 
              maxlength="10" 
              show-word-limit
            />
            <span class="az-helper">显示在开发板用户消息旁</span>
          </el-form-item>

          <el-form-item label="用户头像">
            <div class="avatar-upload-block">
              <div class="avatar-uploader" @click="triggerFileInput">
                <div v-if="userConfig.avatar" class="avatar-preview">
                  <img :src="userConfig.avatar" alt="Avatar">
                  <div class="avatar-hover-overlay">
                    <span v-html="icons.device"></span> 修改
                  </div>
                </div>
                <div v-else class="avatar-placeholder">
                  <span class="avatar-placeholder__plus">+</span>
                  <div class="avatar-placeholder__text">点击上传</div>
                </div>
              </div>

              <input 
                type="file" 
                ref="fileInput" 
                style="display: none;" 
                accept="image/jpeg, image/png" 
                @change="handleFileChange"
              />
              <div class="form-tip">
                支持 JPG/PNG 格式，建议尺寸 100x100，文件大小不超过 50KB。
                <el-button type="danger" text size="small" v-if="userConfig.avatar" @click="userConfig.avatar = ''" style="padding: 0; margin-left: 10px;">清除头像</el-button>
              </div>
            </div>
          </el-form-item>
        </el-form>
        </div>
      </div>
    </div>
  `,
  setup(props) {
    const fileInput = ref(null);

    const userConfig = computed(() => props.config.user || (props.config.user = { username: 'User', avatar: '' }));

    const triggerFileInput = () => {
      if (fileInput.value) {
        fileInput.value.click();
      }
    };

    const handleFileChange = (e) => {
      const file = e.target.files[0];
      if (!file) return;

      if (!['image/jpeg', 'image/png'].includes(file.type)) {
        ElMessage.error('仅支持 JPG/PNG 格式图片');
        e.target.value = null;
        return;
      }
      if (file.size > 50 * 1024) {
        ElMessage.error('图片大小不能超过 50KB');
        e.target.value = null;
        return;
      }

      const reader = new FileReader();
      reader.onload = (event) => {
        userConfig.value.avatar = event.target.result;
      };
      reader.readAsDataURL(file);
      e.target.value = null;
    };

    return {
      userConfig, fileInput, triggerFileInput, handleFileChange
    };
  }
};
