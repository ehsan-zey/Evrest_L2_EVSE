import { useState, type FormEvent } from 'react';
import { api, setToken, ApiError, type User } from '../lib/api';

export function Login({ onSignedIn }: { onSignedIn: (u: User) => void }) {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  async function submit(e: FormEvent): Promise<void> {
    e.preventDefault();
    setBusy(true);
    setError(null);
    try {
      const res = await api.post<{ token: string; user: User }>('/api/auth/login', { email, password });
      setToken(res.token);
      onSignedIn(res.user);
    } catch (err) {
      setError(err instanceof ApiError ? err.message : 'Could not reach the server');
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="login-page">
      <form className="card login-card" onSubmit={submit}>
        <div className="brand" style={{ padding: '0 0 6px' }}>EV<span>REST</span></div>
        <p className="page-sub" style={{ marginBottom: 20 }}>Charger operations</p>

        {error && <div className="error-box">{error}</div>}

        <label className="field">
          <span className="label-text">Email</span>
          <input type="email" value={email} autoComplete="username" required
                 onChange={(e) => setEmail(e.target.value)} />
        </label>
        <label className="field">
          <span className="label-text">Password</span>
          <input type="password" value={password} autoComplete="current-password" required
                 onChange={(e) => setPassword(e.target.value)} />
        </label>

        <button className="primary" type="submit" disabled={busy} style={{ width: '100%', marginTop: 6 }}>
          {busy ? 'Signing in…' : 'Sign in'}
        </button>
      </form>
    </div>
  );
}
