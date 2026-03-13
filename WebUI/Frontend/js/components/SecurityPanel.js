/**
 * SecurityPanel.js — IP 安全管理
 */
import api from '/static/js/api.js';

const { ref, onMounted } = Vue;
const { ElMessage, ElMessageBox } = ElementPlus;

export default {
  name: 'SecurityPanel',
  props: ['config', 'icons'],
  template: `
    <div>
      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.shield"></span>
        IP 安全
      </div>

      <!-- 封禁 IP -->
      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">封禁 IP</span>
        </div>
        <div class="az-card__body">
          <div style="display:flex;gap:10px;margin-bottom:16px;max-width:600px;">
            <el-input v-model="newBan.ip" placeholder="IP 地址，如 192.168.1.100" style="flex:1;" />
            <el-input v-model="newBan.reason" placeholder="原因（可选）" style="flex:1;" />
            <el-checkbox v-model="newBan.permanent" label="永久" />
            <el-button type="primary" @click="handleBan" :loading="banning">封禁</el-button>
          </div>
        </div>
      </div>

      <!-- 封禁列表 -->
      <div class="az-card">
        <div class="az-card__header">
          <span class="az-card__title">封禁列表</span>
          <el-button size="small" @click="loadBans" :loading="loadingBans">刷新</el-button>
        </div>
        <div class="az-card__body--flush">
          <el-table :data="bans" style="width:100%;" empty-text="暂无封禁记录">
            <el-table-column prop="ip_address" label="IP 地址" width="180" />
            <el-table-column prop="reason" label="原因" />
            <el-table-column label="类型" width="100">
              <template #default="{ row }">
                <el-tag :type="row.is_permanent ? 'danger' : 'warning'" size="small">
                  {{ row.is_permanent ? '永久' : '临时' }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="banned_at" label="封禁时间" width="180" />
            <el-table-column label="操作" width="100">
              <template #default="{ row }">
                <el-button size="small" type="danger" text @click="handleUnban(row.ip_address)">解封</el-button>
              </template>
            </el-table-column>
          </el-table>
        </div>
      </div>
    </div>
  `,

  setup() {
    const bans = ref([]);
    const loadingBans = ref(false);
    const banning = ref(false);
    const newBan = ref({ ip: '', reason: '', permanent: false });

    const loadBans = async () => {
      loadingBans.value = true;
      try {
        const d = await api.getSecurityBans();
        bans.value = d.data || [];
      } catch (e) {
        ElMessage.error('加载失败');
      } finally { loadingBans.value = false; }
    };

    const handleBan = async () => {
      if (!newBan.value.ip.trim()) { ElMessage.warning('请输入 IP 地址'); return; }
      banning.value = true;
      try {
        await api.banIp(newBan.value);
        ElMessage.success('已封禁');
        newBan.value = { ip: '', reason: '', permanent: false };
        await loadBans();
      } catch (e) {
        ElMessage.error('封禁失败');
      } finally { banning.value = false; }
    };

    const handleUnban = async (ip) => {
      try {
        await ElMessageBox.confirm(`确定解封 ${ip}？`, '解封确认', { type: 'warning' });
        await api.unbanIp(ip);
        ElMessage.success('已解封');
        await loadBans();
      } catch (e) {
        if (e !== 'cancel') ElMessage.error('解封失败');
      }
    };

    onMounted(loadBans);

    return { bans, loadingBans, banning, newBan, loadBans, handleBan, handleUnban };
  }
};
