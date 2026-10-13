import { createRequire } from 'node:module';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

// funcity 1.5.0 publishes extensionless relative declaration imports, which
// NodeNext rejects. Keep that packaging detail behind a narrow typed facade;
// the public CommonJS export uses the same documented engine and API.
interface TemplateEngine {
  buildCandidateVariables: (
    variables: Record<string, unknown>
  ) => ReadonlyMap<string, unknown>;
  runScriptOnceToText: (
    script: string,
    props: {
      variables: ReadonlyMap<string, unknown>;
      logs: { type: string; description: string }[];
      sourceId: string;
    }
  ) => Promise<string | undefined>;
}

const engine = createRequire(
  resolve(
    dirname(fileURLToPath(import.meta.url)),
    '../../../tools/codegen/package.json'
  )
)('funcity') as TemplateEngine;

/**
 * Renders generated source with the documented funcity text engine.
 * @param script Template text.
 * @param variables Bound source data and formatting functions.
 * @param sourceId Diagnostic source name.
 * @returns Complete generated text.
 * @remarks Any parser or execution diagnostic fails generation.
 */
export const renderCodeTemplate = async (
  script: string,
  variables: Record<string, unknown>,
  sourceId: string
): Promise<string> => {
  const logs: { type: string; description: string }[] = [];
  const result = await engine.runScriptOnceToText(script, {
    variables: engine.buildCandidateVariables(variables),
    logs,
    sourceId,
  });
  if (result === undefined || logs.length !== 0)
    throw new Error(`Code generation failed: ${JSON.stringify(logs)}`);
  return result;
};
