
import { S, $, safe, safeAsync, log, bus } from './state.js';
import { showLoading, hideLoading, updateLoadingStage } from './ui.js';

const BASE_URL = location.origin;
const el = {
    overlay: $('authOverlay'), username: $('usernameInput'),
    password: $('passwordInput'), error: $('authError'), submit: $('authSubmit')
};

const validUser = u => u?.length >= 3 && u.length <= 32 && /^[a-zA-Z0-9_-]+$/.test(u);
const validPass = p => p?.length >= 8;

export const clearSession = () => { S.username = null; S.authenticated = 0; log.info('AUTH', 'Session cleared'); };

const setError = (msg, focusEl) => {
    el.error.textContent = msg;
    [el.username, el.password].forEach(e => e.classList.toggle('error', e === focusEl));
    if (focusEl) focusEl.focus();
};

export const showAuth = (error = '') => {
    el.username.value = el.password.value = '';
    setError(error, error ? el.username : null);
    el.overlay.classList.add('visible');
    el.submit.disabled = false;
    hideLoading();
    setTimeout(() => el.username.focus(), 100);
    log.info('AUTH', 'Auth shown', { hasError: !!error });
};

export const hideAuth = () => {
    el.overlay.classList.remove('visible');
    el.password.value = '';
    setError('', null);
    log.debug('AUTH', 'Auth hidden');
};

const authHTTP = async (username, password) => {
    log.info('AUTH', 'Authenticating', { username });
    const res = await fetch(`${BASE_URL}/api/auth`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        credentials: 'include', body: JSON.stringify({ username, password })
    });
    const data = await res.json();
    if (!res.ok) {
        const error = res.status === 429
            ? `Too many attempts. Try in ${Math.ceil(data.lockoutSeconds / 60)} min.`
            : data.error || 'Authentication failed';
        log.warn('AUTH', 'Failed', { status: res.status, error });
        throw new Error(error);
    }
    S.username = username; S.authenticated = 1;
    log.info('AUTH', 'Authenticated', { username });
};

export const validateSession = async () => {
    const ok = await safeAsync(async () => {
        const res = await fetch(`${BASE_URL}/api/session`, { credentials: 'include' });
        if (res.ok) {
            const data = await res.json();
            if (data.valid) {
                S.authenticated = 1; S.username = data.username;
                log.info('AUTH', 'Session valid', { username: data.username });
                return true;
            }
        }
        return false;
    }, false, 'AUTH');
    if (!ok) clearSession();
    return ok;
};

// --- Form handlers ---
el.username.addEventListener('input', e => {
    e.target.value = e.target.value.replace(/[^a-zA-Z0-9_-]/g, '').slice(0, 32);
    setError('', null);
});

el.username.addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); el.password.focus(); }
});

el.password.addEventListener('keydown', e => {
    if (e.key === 'Enter' && validUser(el.username.value) && el.password.value.length >= 8) el.submit.click();
});

el.submit.addEventListener('click', async () => {
    const username = el.username.value;
    let password = el.password.value;
    el.password.value = '';
    if (!validUser(username)) { setError('Username must be 3-32 characters', el.username); return; }
    if (!validPass(password)) { setError('Password must be at least 8 characters', el.password); return; }
    el.submit.disabled = true;
    el.error.textContent = 'Authenticating...';
    try {
        await authHTTP(username, password);
        hideAuth();
        showLoading(false);
        updateLoadingStage('Connecting...');
        bus.emit('auth:connect');
    } catch (e) {
        setError(e.message, el.username);
        el.submit.disabled = false;
    } finally { password = ''; }
});

$('disconnectBtn')?.addEventListener('click', () => {
    log.info('NET', 'User disconnect');
    bus.emit('user:disconnect');
    safe(() => fetch(`${BASE_URL}/api/logout`, { method: 'POST', credentials: 'include' }), undefined, 'AUTH');
    clearSession();
    showAuth();
});
