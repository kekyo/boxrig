import {
  connectRemoteAgent as connectOnce,
  type ConnectRemoteAgentOptions,
  type RemoteAgent,
} from 'agent-rover';
import { delay } from 'async-primitives';

/**
 * Waits for a shared physical agent selected from the standard configuration.
 * @param options Agent connection settings and per-operation deadlines. The
 * section defaults to win11 when omitted.
 * @returns The established connection, whose operations retain their failures.
 * @remarks Default busy retries continue until the agent is available. An
 * explicitly bounded retry policy is preserved. The SDK selects and retries
 * candidates from .agent-rover; only an exhausted busy wait is renewed here.
 */
export const connectRemoteAgent = async (
  options: Pick<
    ConnectRemoteAgentOptions,
    'section' | 'timeoutMs' | 'retry' | 'onRetry'
  >
): Promise<RemoteAgent> => {
  const waitForAvailability =
    options.retry === undefined || options.retry === true;
  for (;;) {
    try {
      return await connectOnce({
        ...options,
        section: options.section ?? 'win11',
        // Section diagnostics include endpoints. Keep shared test logs generic.
        onRetry:
          options.onRetry ??
          (() =>
            console.warn('Windows agent is unavailable; continuing to wait.')),
      });
    } catch (error) {
      const code =
        error instanceof Error && 'code' in error ? error.code : undefined;
      if (code === 'AGENT_BUSY' && waitForAvailability) {
        // The SDK's default busy budget covers only one hour of shared use.
        console.warn('Windows agent is still busy; continuing to wait.');
        await delay(10000);
        continue;
      }
      throw error;
    }
  }
};
