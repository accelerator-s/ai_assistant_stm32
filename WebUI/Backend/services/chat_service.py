import logging

logger = logging.getLogger(__name__)


def _build_llm_candidate_urls(base_url: str) -> list[str]:
    url = (base_url or '').strip().rstrip('/')
    if not url:
        return []
    if url.endswith('/chat/completions'):
        return [url]

    candidates = []
    if url.endswith('/v1'):
        candidates.append(url + '/chat/completions')
    else:
        candidates.append(url + '/v1/chat/completions')
        candidates.append(url + '/chat/completions')

    unique = []
    seen = set()
    for item in candidates:
        if item not in seen:
            seen.add(item)
            unique.append(item)
    return unique

def get_llm_reply(session_id: str, prompt: str) -> str:
    """获取 LLM 回复 (简化版)"""
    from ..app import app_state
    import requests

    try:
        config = app_state.get('config')
        db = app_state.get('db')
        
        if not config:
            return "出错了：配置未初始化"

        api_key = config.get('llm.api_key')
        base_url = config.get('llm.base_url')
        model = config.get('llm.model', 'gpt-3.5-turbo')
        system_prompt = config.get('llm.system_prompt', '你是一个智能语音助手。')

        if not api_key or not base_url:
            return "出错了：LLM 相关配置缺失，请在配置页填写 API Key 和 Base URL"

        # 获取历史消息构造上下文
        history = db.get_messages(session_id, limit=10) if db else []
        messages = [{"role": "system", "content": system_prompt}]
        for msg in history:
            role = msg['role'] if msg['role'] in ['user', 'assistant'] else 'assistant'
            messages.append({"role": role, "content": msg['content']})
        
        # 将最新一条作为 user 附加（因为前面 get_messages 里已经把它查进去了，或者这取决于调用顺序。
        # 上游是先 save 还是先调用本函数。当前实现在 chat_routes 里是先 save 了，所以 history 最后一个就是最新 user 消息）
        
        headers = {
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json"
        }
        
        payload = {
            "model": model,
            "messages": messages,
            "max_tokens": int(config.get('llm.max_tokens', 512)) if config else 512,
            "temperature": float(config.get('llm.temperature', 0.7)) if config else 0.7
        }

        # 调用 OpenAI 兼容接口（自动兼容多种 base_url 形态）
        data = None
        last_error = None
        for url in _build_llm_candidate_urls(base_url):
            try:
                resp = requests.post(url, json=payload, headers=headers, timeout=15)
                if resp.status_code in (404, 405):
                    continue
                resp.raise_for_status()
                data = resp.json()
                break
            except Exception as e:
                last_error = e
                continue

        if data is None:
            if last_error:
                raise last_error
            return "出错了：未生成可用的模型请求地址"

        if 'choices' in data and len(data['choices']) > 0:
            return data['choices'][0]['message']['content'].strip()
        else:
            return "出错了：接口返回格式不符合预期"

    except Exception as e:
        logger.exception("LLM 调用失败")
        return f"接口请求失败: {str(e)}"
