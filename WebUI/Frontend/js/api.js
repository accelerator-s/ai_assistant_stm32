/**
 * api.js — Axios 封装
 * 参考 qq_with_openai 的 API 层设计
 */
const { ElMessage } = ElementPlus;

const http = axios.create({
  baseURL: '/api',
  timeout: 30000,
  headers: { 'Content-Type': 'application/json' }
});

/* 401 全局拦截回调 */
let _on401 = null;
export function onUnauthorized(cb) { _on401 = cb; }

http.interceptors.response.use(
  (r) => r,
  (error) => {
    const status = error.response?.status;
    const url = error.config?.url || '';

    // 登录/检查端点的错误由调用者处理
    if (url.includes('/auth/login') || url.includes('/auth/check')) {
      return Promise.reject(error);
    }

    // 401 触发全局未授权回调
    if (status === 401) {
      if (_on401) _on401();
      return Promise.reject(error);
    }

    const msg = error.response?.data?.error
      || error.response?.data?.message
      || error.message
      || '网络异常';
    ElMessage.error(msg);
    return Promise.reject(error);
  }
);

const api = {
  // ── 认证 ──
  login(password)                    { return http.post('/auth/login', { password }).then(r => r.data); },
  logout()                           { return http.post('/auth/logout').then(r => r.data); },
  checkAuth()                        { return http.get('/auth/check').then(r => r.data).catch(() => ({ authenticated: false })); },
  changePassword(old_password, new_password) {
    return http.post('/auth/change-password', { old_password, new_password }).then(r => r.data);
  },

  // ── 配置 ──
  getConfig()          { return http.get('/config').then(r => r.data); },
  saveConfig(data)     { return http.post('/config', data).then(r => r.data); },
  getStatus()          { return http.get('/status').then(r => r.data); },

  // ── 测试 ──
  testDevice()         { return http.post('/test/device').then(r => r.data); },
  testSpeech(params)   { return http.post('/test/speech', params).then(r => r.data); },
  testLLM(params)      { return http.post('/test/llm', params).then(r => r.data); },

  // ── 对话历史 ──
  getSessions(params)                { return http.get('/conversations/sessions', { params }).then(r => r.data); },
  getSessionDetail(sessionId)        { return http.get(`/conversations/sessions/${sessionId}`).then(r => r.data); },
  deleteSession(sessionId)           { return http.delete(`/conversations/sessions/${sessionId}`).then(r => r.data); },
  archiveSession(sessionId)          { return http.post(`/conversations/sessions/${sessionId}/archive`).then(r => r.data); },

  // ── 安全 ──
  getSecurityBans()    { return http.get('/security/bans').then(r => r.data); },
  banIp(data)          { return http.post('/security/bans', data).then(r => r.data); },
  unbanIp(ip)          { return http.delete(`/security/bans/${ip}`).then(r => r.data); },
};

export default api;
