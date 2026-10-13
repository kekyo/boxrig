import {
  connectRemoteAgent as connectOnce,
  type RemoteAgent,
} from 'agent-rover';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { connectRemoteAgent } from './agent-connection.js';

vi.mock('agent-rover', () => ({ connectRemoteAgent: vi.fn() }));

const options = {
  section: 'default',
  timeoutMs: 30000,
};

beforeEach(() => {
  vi.useFakeTimers();
  vi.resetAllMocks();
  vi.spyOn(console, 'warn').mockImplementation(() => {});
});

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
});

it.each([
  ['omitted', {}, 'win11'],
  ['undefined', { section: undefined }, 'win11'],
  ['winxp', { section: 'winxp' }, 'winxp'],
  ['default', { section: 'default' }, 'default'],
] as const)(
  'selects the configured section when %s',
  async (_, settings, section) => {
    const agent = { release: vi.fn() } as unknown as RemoteAgent;
    vi.mocked(connectOnce).mockResolvedValue(agent);
    await expect(
      connectRemoteAgent({ timeoutMs: 30000, ...settings })
    ).resolves.toBe(agent);
    expect(connectOnce).toHaveBeenCalledExactlyOnceWith(
      expect.objectContaining({ section })
    );
  }
);

it('keeps configured endpoints and credentials out of retry notices', async () => {
  const agent = { release: vi.fn() } as unknown as RemoteAgent;
  vi.mocked(connectOnce).mockImplementation(async (settings) => {
    settings?.onRetry?.('private-agent.example.invalid private-test-token');
    return agent;
  });
  await expect(connectRemoteAgent({ timeoutMs: 30000 })).resolves.toBe(agent);
  expect(console.warn).toHaveBeenCalledOnce();
  const notices = vi.mocked(console.warn).mock.calls.flat().join(' ');
  expect(notices).not.toContain('private-agent.example.invalid');
  expect(notices).not.toContain('private-test-token');
});

it.each([undefined, true])(
  'keeps waiting after the SDK busy retry budget is exhausted (retry=%s)',
  async (retry) => {
    const agent = { release: vi.fn() } as unknown as RemoteAgent;
    const busy = Object.assign(new Error('Remote agent is already in use.'), {
      code: 'AGENT_BUSY',
    });
    vi.mocked(connectOnce)
      .mockRejectedValueOnce(busy)
      .mockRejectedValueOnce(busy)
      .mockRejectedValueOnce(busy)
      .mockRejectedValueOnce(busy)
      .mockResolvedValueOnce(agent);
    const settings = { ...options, retry };
    const connection = connectRemoteAgent(settings);
    await Promise.all([
      expect(connection).resolves.toBe(agent),
      vi.advanceTimersByTimeAsync(40000),
    ]);
    expect(connectOnce).toHaveBeenCalledTimes(5);
    expect(connectOnce).toHaveBeenLastCalledWith(
      expect.objectContaining(settings)
    );
    const notices = vi.mocked(console.warn).mock.calls.flat().join(' ');
    expect(notices).not.toContain('private-agent.example.invalid');
    expect(notices).not.toContain('private-test-token');
  },
  5000
);

it.each([false, { maxRetries: 0 }, { maxRetries: 2 }])(
  'preserves an explicitly bounded busy retry policy (%s)',
  async (retry) => {
    const busy = Object.assign(new Error('Remote agent is already in use.'), {
      code: 'AGENT_BUSY',
    });
    vi.mocked(connectOnce).mockRejectedValue(busy);
    const settings = { ...options, retry };
    await expect(connectRemoteAgent(settings)).rejects.toBe(busy);
    expect(connectOnce).toHaveBeenCalledExactlyOnceWith(
      expect.objectContaining(settings)
    );
  },
  5000
);

it.each([
  ['AUTHENTICATION_FAILED', 'Authentication failed.'],
  ['AUTHENTICATION_FAILED', 'Remote connection closed.'],
  ['HANDSHAKE_FAILED', 'Unsupported protocol version.'],
  ['CONNECTION_FAILED', 'getaddrinfo ENOTFOUND private-agent.example.invalid'],
  ['OPERATION_FAILED', 'read ECONNRESET'],
  ['DISCONNECTED', 'read ECONNRESET'],
])(
  'preserves %s: %s without retrying',
  async (code, message) => {
    const error = Object.assign(new Error(message), { code });
    vi.mocked(connectOnce).mockRejectedValue(error);
    await expect(connectRemoteAgent(options)).rejects.toBe(error);
    expect(connectOnce).toHaveBeenCalledTimes(1);
  },
  5000
);

it('leaves failures on an established connection visible without replay', async () => {
  const error = Object.assign(new Error('read ECONNRESET'), {
    code: 'DISCONNECTED',
  });
  const capabilities = vi.fn().mockRejectedValue(error);
  const agent = { capabilities } as unknown as RemoteAgent;
  vi.mocked(connectOnce).mockResolvedValue(agent);
  const connected = await connectRemoteAgent(options);
  await expect(connected.capabilities()).rejects.toBe(error);
  expect(connectOnce).toHaveBeenCalledTimes(1);
  expect(capabilities).toHaveBeenCalledTimes(1);
}, 5000);
