/**
 * LoginPage.js — 全屏登录页 (Azure Portal 风格)
 */
import api from '/static/js/api.js';

const { ref } = Vue;
const { ElMessage } = ElementPlus;

export default {
  name: 'LoginPage',
  props: ['onLogin'],
  template: `
    <div class="login-page">
      <div class="login-card">
        <div class="login-card__bar"></div>
        <div class="login-card__header">
          <div class="login-card__logo">
            <svg viewBox="0 0 40 40" width="40" height="40">
              <rect width="40" height="40" rx="8" fill="#0078d4"/>
              <text x="20" y="27" text-anchor="middle" fill="#fff" font-size="18" font-weight="700" font-family="Segoe UI,sans-serif">AI</text>
            </svg>
          </div>
          <div class="login-card__title">STM32 语音助手</div>
          <div class="login-card__subtitle">请输入管理员密码登录</div>
        </div>
        <el-form @submit.prevent="handleLogin" class="login-card__form">
          <el-form-item>
            <el-input
              v-model="password"
              type="password"
              placeholder="管理员密码"
              show-password
              size="large"
              @keyup.enter="handleLogin"
              autofocus
            >
              <template #prefix>
                <svg viewBox="0 0 16 16" width="16" height="16" style="fill:var(--color-text-tertiary);">
                  <rect x="3" y="7" width="10" height="7" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.2"/>
                  <path d="M5.5 7V5a2.5 2.5 0 015 0v2" fill="none" stroke="currentColor" stroke-width="1.2"/>
                </svg>
              </template>
            </el-input>
          </el-form-item>
          <el-button
            type="primary"
            size="large"
            style="width:100%;height:42px;font-weight:600;"
            :loading="loading"
            @click="handleLogin"
          >登 录</el-button>
        </el-form>
      </div>
    </div>
  `,
  setup(props) {
    const password = ref('');
    const loading = ref(false);

    const handleLogin = async () => {
      const pw = (password.value || '').trim();
      if (!pw) { ElMessage.warning('请输入密码'); return; }
      loading.value = true;
      try {
        const resp = await api.login(pw);
        if (resp?.success) {
          ElMessage.success('登录成功');
          props.onLogin?.();
        }
      } catch (err) {
        if (err.response?.status === 422) {
          ElMessage.error('请求格式错误');
        } else {
          const msg = err.response?.data?.error || '登录失败';
          ElMessage.error(msg);
        }
      } finally {
        loading.value = false;
      }
    };

    return { password, loading, handleLogin };
  }
};
