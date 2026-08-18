import { useEffect, useState } from 'react';
import { HashRouter, Routes, Route, NavLink, Navigate, useNavigate } from 'react-router-dom';
import { api, getToken, setToken, type User } from './lib/api';
import { live } from './lib/live';
import { Login } from './pages/Login';
import { Overview } from './pages/Overview';
import { ChargerDetailPage } from './pages/ChargerDetail';
import { Transactions } from './pages/Transactions';
import { Tags } from './pages/Tags';

/**
 * HashRouter rather than BrowserRouter: the dashboard is served as static files
 * next to the API, and hash routing means deep links work without the server
 * needing a catch-all rewrite rule.
 */
export function App() {
  return (
    <HashRouter>
      <Shell />
    </HashRouter>
  );
}

function Shell() {
  const [user, setUser] = useState<User | null>(null);
  const [checking, setChecking] = useState(true);

  useEffect(() => {
    if (!getToken()) { setChecking(false); return; }
    api.get<User>('/api/auth/me')
      .then(setUser)
      .catch(() => setToken(null))
      .finally(() => setChecking(false));
  }, []);

  if (checking) {
    return <div className="login-page"><div className="muted">Loading…</div></div>;
  }

  if (!user) {
    return (
      <Routes>
        <Route path="/login" element={<Login onSignedIn={setUser} />} />
        <Route path="*" element={<Navigate to="/login" replace />} />
      </Routes>
    );
  }

  return (
    <div className="app">
      <Sidebar user={user} onSignOut={() => { setToken(null); live.reset(); setUser(null); }} />
      <main className="main">
        <Routes>
          <Route path="/" element={<Overview />} />
          <Route path="/chargers/:id" element={<ChargerDetailPage role={user.role} />} />
          <Route path="/transactions" element={<Transactions />} />
          <Route path="/tags" element={<Tags role={user.role} />} />
          <Route path="*" element={<Navigate to="/" replace />} />
        </Routes>
      </main>
    </div>
  );
}

function Sidebar({ user, onSignOut }: { user: User; onSignOut: () => void }) {
  const navigate = useNavigate();
  return (
    <nav className="sidebar">
      <div className="brand">EV<span>REST</span></div>
      <NavLink to="/" end className={({ isActive }) => `nav-link${isActive ? ' active' : ''}`}>
        Overview
      </NavLink>
      <NavLink to="/transactions" className={({ isActive }) => `nav-link${isActive ? ' active' : ''}`}>
        Sessions
      </NavLink>
      {/* Tag management is an operator function; hiding it from drivers keeps
          the navigation honest rather than showing a link that 403s. */}
      {user.role !== 'driver' && (
        <NavLink to="/tags" className={({ isActive }) => `nav-link${isActive ? ' active' : ''}`}>
          RFID tags
        </NavLink>
      )}
      <div className="sidebar-footer">
        <div className="small muted" style={{ padding: '0 10px 8px' }}>
          {user.email}<br />
          <span style={{ textTransform: 'capitalize' }}>{user.role}</span>
        </div>
        <button style={{ width: '100%' }} onClick={() => { onSignOut(); navigate('/login'); }}>
          Sign out
        </button>
      </div>
    </nav>
  );
}
