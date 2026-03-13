/**
 * SecuritySettings.js — 密码管理与会话安全
 */
import api from '/static/js/api.js';

const { ref, computed } = Vue;
const { ElMessage } = ElementPlus;

export default {
  name: 'SecuritySettings',
  props: ['config', 'icons'],
  template: `
    <div>
      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.lock"></span>
        密码管理
      </div>

      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">修改管理员密码</span>
        </div>
        <div class="az-card__body">
          <el-form label-width="100px" class="az-form" style="max-width:440px;">
            <el-form-item label="当前密码">
              <el-input v-model="form.oldPassword" type="password" show-password placeholder="输入当前密码" />
            </el-form-item>
            <el-form-item label="新密码">
              <el-input v-model="form.newPassword" type="password" show-password placeholder="至少 8 位" />
            </el-form-item>
            <el-form-item label="确认密码">
              <el-input v-model="form.confirmPassword" type="password" show-password placeholder="再次输入新密码" @keyup.enter="changePassword" />
            </el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="loading" @click="changePassword">修改密码</el-button>
            </el-form-item>
          </el-form>
        </div>
      </div>

      <div class="az-card" style="margin-top:16px;">
        <div class="az-card__header">
          <span class="az-card__title">会话信息</span>
        </div>
        <div class="az-card__body">
          <el-form label-width="120px" class="az-form" style="max-width:480px;">
            <el-form-item label="会话有效时长">
              <el-select v-model="config.advanced.session_expiry_hours" placeholder="选择" style="width:220px;">
                <el-option :value="1"    label="1 小时" />
                <el-option :value="6"    label="6 小时" />
                <el-option :value="12"   label="12 小时" />
                <el-option :value="24"   label="24 小时（默认）" />
                <el-option :value="48"   label="48 小时" />
                <el-option :value="168"  label="7 天" />
                <el-option :value="720"  label="30 天" />
                <el-option :value="0"    label="永不过期" />
              </el-select>
            </el-form-item>
            <div v-if="config.advanced.session_expiry_hours === 0" style="display:flex;align-items:center;gap:6px;padding:8px 12px;background:var(--color-warning-bg);border:1px solid #e8d44d;border-radius:var(--radius-sm);margin-bottom:16px;">
              <el-icon style="color:#b7950b;flex-shrink:0;" :size="16"><WarningFilled /></el-icon>
              <span style="font-size:12px;color:#8a6d00;line-height:1.3;">
                永不过期的会话存在安全风险，建议仅在可信环境中使用此选项。
              </span>
            </div>
          </el-form>
        </div>
      </div>
    </div>
  `,

  setup() {
    const form = ref({ oldPassword: '', newPassword: '', confirmPassword: '' });
    const loading = ref(false);

    const changePassword = async () => {
      if (!form.value.oldPassword) { ElMessage.warning('请输入当前密码'); return; }
      if (form.value.newPassword.length < 8) { ElMessage.warning('新密码至少 8 位'); return; }
      if (form.value.newPassword !== form.value.confirmPassword) { ElMessage.warning('两次输入的密码不一致'); return; }

      loading.value = true;
      try {
        await api.changePassword(form.value.oldPassword, form.value.newPassword);
        ElMessage.success('密码修改成功，请重新登录');
        form.value = { oldPassword: '', newPassword: '', confirmPassword: '' };
        setTimeout(() => { window.location.reload(); }, 1500);
      } catch (e) {
        const msg = e.response?.data?.error || '修改失败';
        ElMessage.error(msg);
      } finally { loading.value = false; }
    };

    return { form, loading, changePassword };
  }
};
