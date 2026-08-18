import { useEffect, useState, type FormEvent } from 'react';
import { api, ApiError, type Tag } from '../lib/api';

export function Tags({ role }: { role: 'admin' | 'operator' | 'driver' }) {
  const [tags, setTags] = useState<Tag[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [idTag, setIdTag] = useState('');
  const [label, setLabel] = useState('');
  const [parent, setParent] = useState('');
  const [busy, setBusy] = useState(false);
  const canEdit = role !== 'driver';

  function load(): void {
    api.get<Tag[]>('/api/tags').then(setTags).catch((e) => setError(e.message));
  }
  useEffect(load, []);

  async function add(e: FormEvent): Promise<void> {
    e.preventDefault();
    setBusy(true);
    setError(null);
    try {
      await api.post('/api/tags', {
        idTag: idTag.trim(),
        label: label.trim() || undefined,
        parentIdTag: parent.trim() || undefined,
      });
      setIdTag(''); setLabel(''); setParent('');
      load();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : String(err));
    } finally {
      setBusy(false);
    }
  }

  async function setStatus(tag: Tag, status: Tag['status']): Promise<void> {
    try {
      await api.post('/api/tags', {
        idTag: tag.id_tag,
        label: tag.label ?? undefined,
        parentIdTag: tag.parent_id_tag ?? undefined,
        status,
      });
      load();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : String(err));
    }
  }

  return (
    <>
      <div className="page-head">
        <div>
          <h1 className="page-title">RFID tags</h1>
          <p className="page-sub">
            Only tags listed here can start a session. An unknown tag is refused.
          </p>
        </div>
      </div>

      {error && <div className="error-box">{error}</div>}

      {canEdit && (
        <div className="card" style={{ marginBottom: 16 }}>
          <h2 className="card-title">Add or update a tag</h2>
          <form onSubmit={add} className="row" style={{ alignItems: 'flex-end' }}>
            <label className="field" style={{ flex: '1 1 180px', marginBottom: 0 }}>
              <span className="label-text">Tag id</span>
              <input value={idTag} required maxLength={20} placeholder="04A2B3C4D5"
                     onChange={(e) => setIdTag(e.target.value)} />
            </label>
            <label className="field" style={{ flex: '1 1 180px', marginBottom: 0 }}>
              <span className="label-text">Label</span>
              <input value={label} placeholder="Fleet van 3"
                     onChange={(e) => setLabel(e.target.value)} />
            </label>
            <label className="field" style={{ flex: '1 1 180px', marginBottom: 0 }}>
              <span className="label-text">Group (parent tag)</span>
              <input value={parent} maxLength={20} placeholder="optional"
                     onChange={(e) => setParent(e.target.value)} />
            </label>
            <button className="primary" type="submit" disabled={busy}>
              {busy ? 'Saving…' : 'Save'}
            </button>
          </form>
          <div className="small muted" style={{ marginTop: 10 }}>
            Tags sharing a group can stop each other's sessions — useful for a shared
            fleet vehicle, and the reason a group is opt-in rather than the default.
          </div>
        </div>
      )}

      <div className="card">
        {tags.length === 0 ? (
          <div className="empty">No tags yet</div>
        ) : (
          <table className="data">
            <thead>
              <tr>
                <th>Tag</th><th>Label</th><th>Group</th><th>Status</th>
                {canEdit && <th></th>}
              </tr>
            </thead>
            <tbody>
              {tags.map((t) => (
                <tr key={t.id_tag}>
                  <td className="mono">{t.id_tag}</td>
                  <td>{t.label ?? <span className="muted">—</span>}</td>
                  <td className="mono small">{t.parent_id_tag ?? <span className="muted">—</span>}</td>
                  <td>
                    <span className={`badge ${t.status === 'Accepted' ? 'st-charging' : 'st-faulted'}`}>
                      <span className="dot" />
                      <span className="glyph">{t.status === 'Accepted' ? '✓' : '✕'}</span>
                      {t.status}
                    </span>
                  </td>
                  {canEdit && (
                    <td style={{ textAlign: 'right' }}>
                      <div className="row" style={{ justifyContent: 'flex-end' }}>
                        {t.status === 'Accepted' ? (
                          <button onClick={() => setStatus(t, 'Blocked')}>Block</button>
                        ) : (
                          <button onClick={() => setStatus(t, 'Accepted')}>Allow</button>
                        )}
                        <button className="danger" onClick={() => {
                          if (!confirm(`Delete tag ${t.id_tag}? Past sessions keep their record.`)) return;
                          api.del(`/api/tags/${encodeURIComponent(t.id_tag)}`).then(load)
                            .catch((e) => setError(e.message));
                        }}>Delete</button>
                      </div>
                    </td>
                  )}
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
    </>
  );
}
