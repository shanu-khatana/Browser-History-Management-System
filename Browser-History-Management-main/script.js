// script.js (complete — uses server SQLite backend; preserves all original UI behaviors)
// API server expected: http://localhost:8080

const API_BASE_URL = 'http://localhost:8080';

// DOM Elements (assumes same IDs/classes as original project)
const navLinks = document.querySelectorAll('.nav-link');
const sections = document.querySelectorAll('.section');
const notification = document.getElementById('notification');
const notificationText = document.getElementById('notificationText');

const urlInput = document.getElementById('urlInput');
const visitBtn = document.getElementById('visitBtn');
const backBtn = document.getElementById('backBtn');
const forwardBtn = document.getElementById('forwardBtn');
const clearBtn = document.getElementById('clearBtn');
const searchInput = document.getElementById('searchInput');
const searchBtn = document.getElementById('searchBtn');
const historyList = document.getElementById('historyList');
const searchResults = document.getElementById('searchResults');
const bookmarksList = document.getElementById('bookmarksList');

const sortSelect = document.getElementById('sortSelect');
const charFilter = document.getElementById('charFilter');

const homeSearchInput = document.getElementById('homeSearchInput');
const homeSearchBtn = document.getElementById('homeSearchBtn');
const totalVisitsEl = document.getElementById('totalVisits');
const uniqueUrlsEl = document.getElementById('uniqueUrls');
const currentPositionEl = document.getElementById('currentPosition');

const loginForm = document.getElementById('loginForm');
const toggleAuthMode = document.getElementById('toggleAuthMode');
const authTitle = document.getElementById('authTitle');
const authSubtitle = document.getElementById('authSubtitle');
const authSubmit = document.getElementById('authSubmit');
const logoutBox = document.getElementById('logoutBox');
const logoutBtn = document.getElementById('logoutBtn');
const userDisplay = document.getElementById('userDisplay');

let authMode = 'login'; // or 'register'
let currentHistory = []; // holds last fetched history array

// -------------------- Navigation --------------------
navLinks.forEach(link => {
    link.addEventListener('click', (e) => {
        e.preventDefault();
        const targetSection = link.getAttribute('data-section');

        navLinks.forEach(l => l.classList.remove('active'));
        link.classList.add('active');

        sections.forEach(section => {
            section.classList.remove('active');
            if (section.id === targetSection) {
                section.classList.add('active');
            }
        });

        // Persist last open section so returning tab restores UI exactly
        try { sessionStorage.setItem('lastSection', targetSection); } catch (err) { /* ignore */ }

        if (targetSection === 'search') {
            loadHistory();
            loadBookmarksUI();
        }
        if (targetSection === 'home') {
            updateStats();
        }
    });
});

// -------------------- Notifications --------------------
function showNotification(message, type = 'success') {
    if (!notification || !notificationText) return;
    notificationText.textContent = message;
    notification.className = `notification ${type} show`;

    const icon = notification.querySelector('i');
    if (icon) {
        if (type === 'success') icon.className = 'fas fa-check-circle';
        else icon.className = 'fas fa-exclamation-circle';
    }

    setTimeout(() => { if (notification) notification.classList.remove('show'); }, 3000);
}

// -------------------- Auth helpers (client-side, unchanged behavior) --------------------
function getCurrentUser() {
    return localStorage.getItem('currentUser') || null;
}
function registerUser(username, password) {
    const raw = localStorage.getItem('users');
    const users = raw ? JSON.parse(raw) : {};
    if (users[username]) return { ok: false, message: 'Username already exists' };
    users[username] = { password }; // demo only
    localStorage.setItem('users', JSON.stringify(users));
    return { ok: true };
}
function loginUser(username, password) {
    const raw = localStorage.getItem('users');
    const users = raw ? JSON.parse(raw) : {};
    if (!users[username]) return { ok: false, message: 'User not found' };
    if (users[username].password !== password) return { ok: false, message: 'Incorrect password' };
    localStorage.setItem('currentUser', username);
    return { ok: true };
}
function logoutUser() {
    localStorage.removeItem('currentUser');
}
function updateAuthUI() {
    const user = getCurrentUser();
    if (!authTitle || !authSubtitle || !loginForm || !logoutBox || !userDisplay) return;

    if (user) {
        authTitle.textContent = 'Logged in';
        authSubtitle.textContent = `Welcome, ${user}`;
        loginForm.style.display = 'none';
        logoutBox.style.display = 'block';
        userDisplay.textContent = `Signed in: ${user}`;
    } else {
        authTitle.textContent = (authMode === 'login') ? 'Login' : 'Register';
        authSubtitle.textContent = (authMode === 'login') ? 'Please login to access your personalized features.' : 'Create a new account.';
        loginForm.style.display = 'block';
        logoutBox.style.display = 'none';
        userDisplay.textContent = '';
        try { loginForm.reset(); } catch (e) {}
    }
}

if (toggleAuthMode) {
    toggleAuthMode.addEventListener('click', (e) => {
        e.preventDefault();
        authMode = (authMode === 'login') ? 'register' : 'login';
        toggleAuthMode.textContent = (authMode === 'login') ? "Don't have an account? Register" : "Already have an account? Login";
        authTitle.textContent = (authMode === 'login') ? 'Login' : 'Register';
        authSubmit.innerHTML = (authMode === 'login') ? '<i class="fas fa-sign-in-alt"></i> Login' : '<i class="fas fa-user-plus"></i> Register';
        updateAuthUI();
    });
}

if (loginForm) {
    loginForm.addEventListener('submit', (e) => {
        e.preventDefault();
        const username = document.getElementById('username').value.trim();
        const password = document.getElementById('password').value;
        if (!username || !password) { showNotification('Enter username and password', 'error'); return; }

        if (authMode === 'login') {
            const res = loginUser(username, password);
            if (!res.ok) { showNotification(res.message, 'error'); return; }
            showNotification(`Login successful. Welcome ${username}!`);
            updateAuthUI();
            loadBookmarksUI();
            loadHistory();
            updateStats();
        } else {
            const res = registerUser(username, password);
            if (!res.ok) { showNotification(res.message, 'error'); return; }
            showNotification('Registration successful. You can now login.');
            authMode = 'login';
            toggleAuthMode.textContent = "Don't have an account? Register";
            updateAuthUI();
        }
    });
}

if (logoutBtn) {
    logoutBtn.addEventListener('click', () => {
        logoutUser();
        showNotification('Logged out');
        updateAuthUI();
        loadBookmarksUI();
        loadHistory();
        updateStats();
    });
}

// -------------------- Utilities --------------------
function escapeHtml(s) {
    return (s + '').replace(/[&<>"']/g, (m) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));
}
function requireLogin(actionName = 'perform this action') {
    if (!getCurrentUser()) {
        showNotification(`Please login to ${actionName}`, 'error');
        return false;
    }
    return true;
}

// -------------------- Server API wrappers --------------------
async function apiGet(path) {
    const res = await fetch(API_BASE_URL + path, { method: 'GET', headers: {'Accept':'application/json'} });
    return res;
}
async function apiPost(path, bodyObj) {
    const body = new URLSearchParams(bodyObj).toString();
    const res = await fetch(API_BASE_URL + path, {
        method: 'POST',
        headers: {'Content-Type':'application/x-www-form-urlencoded'},
        body
    });
    return res;
}
async function apiDelete(path) {
    const res = await fetch(API_BASE_URL + path, { method: 'DELETE' });
    return res;
}

// -------------------- History (server-backed) --------------------
async function loadHistory() {
    const user = getCurrentUser();
    if (!user) {
        currentHistory = [];
        renderHistoryList();
        return;
    }
    try {
        const res = await apiGet(`/history?user=${encodeURIComponent(user)}`);
        const json = await res.json();
        currentHistory = json.history || [];
        renderHistoryList();
    } catch (e) {
        console.error(e);
        showNotification('Failed to load history from server', 'error');
    }
}

function applyFiltersAndSort(arr) {
    let out = arr.slice();

    const ch = (charFilter && charFilter.value) ? (charFilter.value.trim().toLowerCase()) : '';
    if (ch) {
        out = out.filter(it => it.url && it.url[0] && it.url[0].toLowerCase() === ch);
    }

    const mode = (sortSelect && sortSelect.value) ? sortSelect.value : 'recent';
    if (mode === 'az') {
        out.sort((a,b) => a.url.localeCompare(b.url));
    } else if (mode === 'za') {
        out.sort((a,b) => b.url.localeCompare(a.url));
    } else if (mode === 'vis-high') {
        out.sort((a,b) => b.visits - a.visits);
    } else if (mode === 'vis-low') {
        out.sort((a,b) => a.visits - b.visits);
    } else if (mode === 'oldest') {
        // server returns oldest->newest, so keep as-is
    } else { // recent (default)
        out = out.slice().reverse();
    }

    return out;
}

function renderHistoryList() {
    if (!historyList) return;
    historyList.innerHTML = '';

    if (!getCurrentUser()) {
        historyList.innerHTML = '<li class="history-item">Please login to view your history</li>';
        return;
    }

    if (!currentHistory || currentHistory.length === 0) {
        historyList.innerHTML = '<li class="history-item">No history yet. Start by visiting a URL!</li>';
        return;
    }

    const processed = applyFiltersAndSort(currentHistory);

    processed.forEach((item, index) => {
        const li = document.createElement('li');
        li.className = `history-item ${item.current ? 'current' : ''}`;
        li.innerHTML = `
            <div style="flex:1;">
                <div class="history-url">${escapeHtml(item.url)}</div>
                <div class="history-meta" style="margin-top:6px;font-size:0.9rem;color:#555;">
                    <span><i class="fas fa-eye"></i> ${item.visits} visits</span>
                </div>
            </div>
            <div style="display:flex;gap:8px;align-items:center;">
                <button class="small" data-action="visit" data-url="${encodeURIComponent(item.url)}"><i class="fas fa-external-link-alt"></i></button>
                <button class="small" data-action="bookmark" data-url="${encodeURIComponent(item.url)}"><i class="fas fa-bookmark"></i></button>
            </div>
        `;
        historyList.appendChild(li);
    });
    attachHistoryItemHandlers();
}

function attachHistoryItemHandlers() {
    if (!historyList) return;
    historyList.querySelectorAll('button[data-action]').forEach(btn => {
        const action = btn.dataset.action;
        const url = decodeURIComponent(btn.dataset.url || '');
        btn.addEventListener('click', (e) => {
            if (action === 'visit') openAndRecordUrl(url);
            else if (action === 'bookmark') toggleBookmark(url);
        });
    });
}

// -------------------- Bookmarks (server-backed) --------------------
async function loadBookmarks() {
    const user = getCurrentUser();
    if (!user) return [];
    try {
        const res = await apiGet(`/bookmarks?user=${encodeURIComponent(user)}`);
        const json = await res.json();
        return (json.bookmarks || []).map(b => b.url);
    } catch (e) {
        console.error(e);
        return [];
    }
}

async function loadBookmarksUI() {
    if (!bookmarksList) return;
    bookmarksList.innerHTML = '';

    if (!getCurrentUser()) {
        bookmarksList.innerHTML = '<li class="history-item">Please login to view bookmarks</li>';
        return;
    }

    const bm = await loadBookmarks();
    if (bm.length === 0) {
        bookmarksList.innerHTML = '<li class="history-item">No bookmarks yet</li>';
        return;
    }
    bm.forEach(url => {
        const li = document.createElement('li');
        li.className = 'history-item';
        li.innerHTML = `
            <div style="flex:1;">
                <div class="history-url">${escapeHtml(url)}</div>
            </div>
            <div style="display:flex;gap:8px;align-items:center;">
                <button class="small" data-action="visit" data-url="${encodeURIComponent(url)}"><i class="fas fa-external-link-alt"></i></button>
                <button class="small danger" data-action="remove" data-url="${encodeURIComponent(url)}"><i class="fas fa-trash"></i></button>
            </div>
        `;
        bookmarksList.appendChild(li);
    });

    bookmarksList.querySelectorAll('button[data-action]').forEach(btn => {
        btn.addEventListener('click', async () => {
            const url = decodeURIComponent(btn.dataset.url || '');
            if (btn.dataset.action === 'visit') openAndRecordUrl(url);
            else if (btn.dataset.action === 'remove') {
                const user = getCurrentUser();
                if (!user) { showNotification('Please login', 'error'); return; }
                try {
                    await apiDelete(`/bookmarks?user=${encodeURIComponent(user)}&url=${encodeURIComponent(url)}`);
                    showNotification('Bookmark removed');
                    loadBookmarksUI();
                } catch (e) {
                    console.error(e);
                    showNotification('Failed to remove bookmark', 'error');
                }
            }
        });
    });
}

async function toggleBookmark(url) {
    if (!requireLogin('manage bookmarks')) return;
    const user = getCurrentUser();
    try {
        const bm = await loadBookmarks();
        if (bm.includes(url)) {
            await apiDelete(`/bookmarks?user=${encodeURIComponent(user)}&url=${encodeURIComponent(url)}`);
            showNotification('Removed bookmark');
        } else {
            await apiPost('/bookmarks', { user, url });
            showNotification('Added bookmark');
        }
        loadBookmarksUI();
    } catch (e) {
        console.error(e);
        showNotification('Bookmark action failed', 'error');
    }
}

// -------------------- Visit / navigation operations (server-backed) --------------------
async function openAndRecordUrl(rawUrl) {
    if (!requireLogin('visit URLs')) return;
    if (!rawUrl) return;
    let url = rawUrl.trim();
    if (!/^https?:\/\//i.test(url)) url = 'http://' + url;

    const user = getCurrentUser();
    if (!user) return;

    try {
        await apiPost('/visit', { url, user });
        // opens in new tab (preserve original tab), as requested by user
        window.open(url, '_blank');
        showNotification(`Opened ${url}`);
        await loadHistory();
        await updateStats();
    } catch (e) {
        console.error(e);
        showNotification('Failed to record visit', 'error');
    }
}

if (visitBtn) {
    visitBtn.addEventListener('click', () => {
        if (!requireLogin('visit URLs')) return;
        const url = urlInput.value.trim();
        if (!url) { showNotification('Please enter a URL', 'error'); return; }
        openAndRecordUrl(url);
        urlInput.value = '';
    });
}

if (backBtn) {
    backBtn.addEventListener('click', async () => {
        if (!requireLogin('navigate history')) return;
        const user = getCurrentUser();
        try {
            const res = await apiPost('/back', { user });
            const json = await res.json();
            if (json.status === 'success') {
                window.open(json.url, '_blank');
                showNotification(`Navigated back to: ${json.url}`);
                await loadHistory();
                updateStats();
            } else {
                showNotification(json.message || 'No previous page', 'error');
            }
        } catch (e) {
            console.error(e);
            showNotification('Failed to navigate back', 'error');
        }
    });
}

if (forwardBtn) {
    forwardBtn.addEventListener('click', async () => {
        if (!requireLogin('navigate history')) return;
        const user = getCurrentUser();
        try {
            const res = await apiPost('/forward', { user });
            const json = await res.json();
            if (json.status === 'success') {
                window.open(json.url, '_blank');
                showNotification(`Navigated forward to: ${json.url}`);
                await loadHistory();
                updateStats();
            } else {
                showNotification(json.message || 'No next page', 'error');
            }
        } catch (e) {
            console.error(e);
            showNotification('Failed to navigate forward', 'error');
        }
    });
}

if (clearBtn) {
    clearBtn.addEventListener('click', async () => {
        if (!requireLogin('clear history')) return;
        if (!confirm('Are you sure you want to clear all history?')) return;
        const user = getCurrentUser();
        try {
            await apiPost('/clear', { user });
            showNotification('History cleared');
            await loadHistory();
            updateStats();
        } catch (e) {
            console.error(e);
            showNotification('Failed to clear history', 'error');
        }
    });
}

// -------------------- Search --------------------
if (searchBtn) {
    searchBtn.addEventListener('click', async () => {
        if (!requireLogin('search history')) return;
        const user = getCurrentUser();
        const q = searchInput.value.trim();
        if (!q) { showNotification('Please enter a search query', 'error'); return; }
        try {
            const res = await apiGet(`/search?q=${encodeURIComponent(q)}&user=${encodeURIComponent(user)}`);
            const json = await res.json();
            const results = json.results || [];
            searchResults.innerHTML = '';
            if (results.length === 0) {
                searchResults.innerHTML = '<p>No results found.</p>';
                return;
            }
            const title = document.createElement('h2');
            title.textContent = 'Search Results';
            searchResults.appendChild(title);

            results.forEach(r => {
                const div = document.createElement('div');
                div.className = 'search-result-item';
                div.innerHTML = `
                    <div class="search-result-url">${escapeHtml(r.url)}</div>
                    <div style="margin-top:6px;"><i class="fas fa-eye"></i> Visits: ${r.visits}
                        <button class="small" data-url="${encodeURIComponent(r.url)}" style="margin-left:10px;">Open</button>
                        <button class="small" data-bookmark="${encodeURIComponent(r.url)}" style="margin-left:8px;"><i class="fas fa-bookmark"></i></button>
                    </div>
                `;
                searchResults.appendChild(div);

                div.querySelector('button[data-url]').addEventListener('click', () => openAndRecordUrl(decodeURIComponent(div.querySelector('button[data-url]').dataset.url)));
                div.querySelector('button[data-bookmark]').addEventListener('click', async () => {
                    const url = decodeURIComponent(div.querySelector('button[data-bookmark]').dataset.bookmark);
                    if (!getCurrentUser()) { showNotification('Please login to bookmark', 'error'); return; }
                    const bm = await loadBookmarks();
                    if (bm.includes(url)) { showNotification('Already bookmarked'); }
                    else { await apiPost('/bookmarks', { user: getCurrentUser(), url }); showNotification('Bookmarked'); loadBookmarksUI(); }
                });
            });
        } catch (e) {
            console.error(e);
            showNotification('Search failed', 'error');
        }
    });
}

// Home search shortcut
if (homeSearchBtn) {
    homeSearchBtn.addEventListener('click', () => {
        if (!requireLogin('search')) return;
        const q = homeSearchInput.value.trim();
        if (!q) { showNotification('Please enter a search query', 'error'); return; }
        document.querySelector('[data-section="search"]').click();
        searchInput.value = q;
        searchBtn.click();
    });
}

if (homeSearchInput) homeSearchInput.addEventListener('keypress', (e) => { if (e.key === 'Enter') homeSearchBtn.click(); });
if (searchInput) searchInput.addEventListener('keypress', (e) => { if (e.key === 'Enter') searchBtn.click(); });
if (urlInput) urlInput.addEventListener('keypress', (e) => { if (e.key === 'Enter') visitBtn.click(); });

if (sortSelect) sortSelect.addEventListener('change', renderHistoryList);
if (charFilter) charFilter.addEventListener('input', renderHistoryList);

// -------------------- Stats (server-backed) --------------------
async function updateStats() {
    const user = getCurrentUser();
    if (!user) {
        if (totalVisitsEl) totalVisitsEl.textContent = 0;
        if (uniqueUrlsEl) uniqueUrlsEl.textContent = 0;
        if (currentPositionEl) currentPositionEl.textContent = 0;
        return;
    }
    try {
        const res = await apiGet(`/history?user=${encodeURIComponent(user)}`);
        const json = await res.json();
        const historyData = json.history || [];
        let totalVisits = 0;
        historyData.forEach(item => { totalVisits += (item.visits || 0); });
        if (totalVisitsEl) totalVisitsEl.textContent = totalVisits;
        if (uniqueUrlsEl) uniqueUrlsEl.textContent = historyData.length;
        if (currentPositionEl) currentPositionEl.textContent = (json.current_position >= 0) ? json.current_position + 1 : 0;
    } catch (e) {
        console.error(e);
    }
}

// -------------------- Initial load (restores last section) --------------------
document.addEventListener('DOMContentLoaded', () => {
    updateStats();
    updateAuthUI();

    // restore last section if available (keeps UI identical when user returns from opened link)
    let last = null;
    try { last = sessionStorage.getItem('lastSection'); } catch (err) { last = null; }

    if (last) {
        const targetLink = Array.from(navLinks).find(l => l.getAttribute('data-section') === last);
        if (targetLink) {
            navLinks.forEach(l => l.classList.remove('active'));
            targetLink.classList.add('active');
            sections.forEach(section => {
                section.classList.remove('active');
                if (section.id === last) section.classList.add('active');
            });
        }
    }

    const activeSection = document.querySelector('.nav-link.active') ?
                          document.querySelector('.nav-link.active').getAttribute('data-section') : 'home';

    if (activeSection === 'search') {
        loadHistory();
        loadBookmarksUI();
    } else if (activeSection === 'home') {
        updateStats();
    }
});
