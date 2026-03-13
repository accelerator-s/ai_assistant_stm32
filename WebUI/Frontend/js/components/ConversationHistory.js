/**
 * ConversationHistory.js — 对话历史管理
 */
import api from '/static/js/api.js';

const { ref, onMounted, computed } = Vue;
const { ElMessage, ElMessageBox } = ElementPlus;

export default {
  name: 'ConversationHistory',
  props: ['config', 'icons'],
  template: `
    <div>
      <div class="az-page-title">
        <span class="az-page-title__icon" v-html="icons.message"></span>
        对话历史
      </div>

      <div style="display:flex;gap:16px;height:calc(100vh - 220px);">
        <!-- 会话列表 -->
        <div class="az-card" style="width:320px;min-width:280px;display:flex;flex-direction:column;margin-bottom:0;">
          <div class="az-card__header">
            <span class="az-card__title">会话列表</span>
            <el-button size="small" @click="loadSessions" :loading="loadingSessions">刷新</el-button>
          </div>
          <div class="az-card__body--flush" style="flex:1;overflow-y:auto;">
            <div v-if="sessions.length === 0" style="text-align:center;padding:40px 20px;color:var(--color-text-tertiary);font-size:13px;">
              暂无对话记录
            </div>
            <ul class="az-conv-list" v-else>
              <li v-for="s in sessions" :key="s.id"
                  class="az-conv-item"
                  :class="{ 'az-conv-item--active': selectedId === s.id }"
                  @click="selectSession(s.id)">
                <div>
                  <div class="az-conv-item__title">{{ s.title }}</div>
                  <div class="az-conv-item__meta">{{ s.device_id }} · {{ formatTime(s.updated_at) }}</div>
                </div>
                <el-button size="small" type="danger" text @click.stop="deleteSession(s.id)">
                  删除
                </el-button>
              </li>
            </ul>
          </div>
        </div>

        <!-- 消息详情 -->
        <div class="az-card" style="flex:1;display:flex;flex-direction:column;margin-bottom:0;">
          <div class="az-card__header">
            <span class="az-card__title">{{ currentSession ? currentSession.title : '选择会话查看详情' }}</span>
          </div>
          <div class="az-card__body" style="flex:1;overflow-y:auto;">
            <div v-if="!currentSession" style="text-align:center;padding:60px 20px;color:var(--color-text-tertiary);font-size:13px;">
              请从左侧选择一个会话
            </div>
            <div v-else-if="loadingMessages" style="text-align:center;padding:60px 20px;">
              <div class="az-spinner"></div>
            </div>
            <div v-else>
              <div v-for="msg in messages" :key="msg.id" class="az-chat-msg" :class="'az-chat-msg--' + msg.role">
                <div class="az-chat-msg__role">{{ msg.role === 'user' ? '用户' : (msg.role === 'assistant' ? '助手' : '系统') }}</div>
                {{ msg.content }}
              </div>
              <div v-if="messages.length === 0" style="text-align:center;padding:40px;color:var(--color-text-tertiary);font-size:13px;">
                该会话暂无消息记录
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  `,

  setup() {
    const sessions = ref([]);
    const messages = ref([]);
    const selectedId = ref(null);
    const currentSession = ref(null);
    const loadingSessions = ref(false);
    const loadingMessages = ref(false);

    const loadSessions = async () => {
      loadingSessions.value = true;
      try {
        const d = await api.getSessions({ limit: 100 });
        sessions.value = d.data || [];
      } catch (e) {
        ElMessage.error('加载会话列表失败');
      } finally { loadingSessions.value = false; }
    };

    const selectSession = async (id) => {
      selectedId.value = id;
      loadingMessages.value = true;
      try {
        const d = await api.getSessionDetail(id);
        currentSession.value = d.data.session;
        messages.value = d.data.messages || [];
      } catch (e) {
        ElMessage.error('加载会话详情失败');
      } finally { loadingMessages.value = false; }
    };

    const deleteSession = async (id) => {
      try {
        await ElMessageBox.confirm('确定删除此会话及所有消息？', '删除确认', { type: 'warning' });
        await api.deleteSession(id);
        ElMessage.success('会话已删除');
        if (selectedId.value === id) {
          selectedId.value = null;
          currentSession.value = null;
          messages.value = [];
        }
        await loadSessions();
      } catch (e) {
        if (e !== 'cancel') ElMessage.error('删除失败');
      }
    };

    const formatTime = (t) => {
      if (!t) return '';
      return t.replace('T', ' ').substring(0, 19);
    };

    onMounted(loadSessions);

    return { sessions, messages, selectedId, currentSession, loadingSessions, loadingMessages, loadSessions, selectSession, deleteSession, formatTime };
  }
};
