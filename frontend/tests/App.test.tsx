import { act, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import { App } from '@/App';
import { api, ApiError, clearToken, setToken } from '@/api/client';

vi.mock('@/components/Dashboard', () => ({ Dashboard: () => <main>Household dashboard</main> }));

const user = { id: 'u1', name: 'Ada', email: 'ada@example.com', role: 'admin' as const };

beforeEach(() => {
  vi.restoreAllMocks();
  clearToken();
});

describe('session handling', () => {
  it('validates a stored token before rendering the dashboard', async () => {
    setToken('stored-token');
    let resolve!: (value: typeof user) => void;
    vi.spyOn(api, 'me').mockReturnValue(
      new Promise((done) => {
        resolve = done;
      }),
    );
    render(<App />);
    expect(screen.queryByText('Household dashboard')).not.toBeInTheDocument();
    act(() => {
      resolve(user);
    });
    await screen.findByText('Household dashboard');
    expect(screen.getByText('Household dashboard')).toBeInTheDocument();
    expect(screen.getByText(user.email)).toBeInTheDocument();
  });

  it('returns an invalid stored session to sign-in', async () => {
    setToken('invalid-token');
    vi.spyOn(api, 'me').mockRejectedValue(new ApiError(401, 'authentication required'));
    render(<App />);
    expect(await screen.findByRole('button', { name: 'Sign in' })).toBeInTheDocument();
    expect(screen.queryByText('Household dashboard')).not.toBeInTheDocument();
  });

  it('returns to sign-in when an active session is lost', async () => {
    setToken('stored-token');
    vi.spyOn(api, 'me').mockResolvedValue(user);
    render(<App />);
    await screen.findByText('Household dashboard');
    act(() => {
      window.dispatchEvent(new Event('homestr:unauthorized'));
    });
    expect(screen.getByRole('button', { name: 'Sign in' })).toBeInTheDocument();
  });

  it('keeps a temporary validation failure retryable', async () => {
    setToken('stored-token');
    const me = vi
      .spyOn(api, 'me')
      .mockRejectedValueOnce(new Error('offline'))
      .mockResolvedValue(user);
    render(<App />);
    await userEvent.click(await screen.findByRole('button', { name: 'Retry' }));
    await screen.findByText('Household dashboard');
    expect(me).toHaveBeenCalledTimes(2);
  });

  it('waits for server logout before ending the session', async () => {
    setToken('stored-token');
    vi.spyOn(api, 'me').mockResolvedValue(user);
    const logout = vi.spyOn(api, 'logout').mockResolvedValue();
    render(<App />);
    await userEvent.click(await screen.findByRole('button', { name: 'Sign out' }));
    await waitFor(() => expect(logout).toHaveBeenCalledOnce());
    expect(await screen.findByRole('button', { name: 'Sign in' })).toBeInTheDocument();
  });
});
