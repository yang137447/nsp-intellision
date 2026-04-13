import type { ReplaySampleSnapshot, ReplayStep } from './real_workspace_replay_types';

export function detectReplayAnomalies(step: ReplayStep, samples: ReplaySampleSnapshot[]): string[] {
	const anomalies: string[] = [];
	const windowLabel = step.samplingWindow?.label.toLowerCase() ?? '';
	if (windowLabel.includes('completion') || windowLabel.includes('member')) {
		const sawCompletion = samples.some((sample) => (sample.internalStatus?.completionRequestCount ?? 0) > 0);
		if (!sawCompletion) {
			anomalies.push('completion-request-not-observed');
		}
	}
	if (windowLabel.includes('signature') || (step.kind === 'typeText' && step.payload.text.includes('('))) {
		const sawSignatureHelp = samples.some((sample) => (sample.internalStatus?.signatureHelpRequestCount ?? 0) > 0);
		if (!sawSignatureHelp) {
			anomalies.push('signature-help-request-not-observed');
		}
	}
	if (samples.every((sample) => (sample.internalStatus?.activeRpcCount ?? 0) > 0)) {
		anomalies.push('active-rpc-backlog-never-settled');
	}
	return anomalies;
}
