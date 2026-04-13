import type { ReplaySampleSnapshot, ReplayStep } from './real_workspace_replay_types';

export function detectReplayAnomalies(step: ReplayStep, samples: ReplaySampleSnapshot[]): string[] {
	const anomalies: string[] = [];
	if (samples.length === 0) {
		return anomalies;
	}

	const windowLabel = step.samplingWindow?.label.toLowerCase() ?? '';

	const counterIncreased = (field: string) => {
		let previous: number | undefined;
		for (const sample of samples) {
			const current = sample.internalStatus?.[field];
			if (typeof current !== 'number') {
				continue;
			}
			if (previous !== undefined && current > previous) {
				return true;
			}
			previous = current;
		}
		return false;
	};

	if (windowLabel.includes('completion') || windowLabel.includes('member')) {
		if (!counterIncreased('completionRequestCount')) {
			anomalies.push('completion-request-not-observed');
		}
	}
	if (windowLabel.includes('signature') || (step.kind === 'typeText' && step.payload.text.includes('('))) {
		if (!counterIncreased('signatureHelpRequestCount')) {
			anomalies.push('signature-help-request-not-observed');
		}
	}
	if (samples.every((sample) => (sample.internalStatus?.activeRpcCount ?? 0) > 0)) {
		anomalies.push('active-rpc-backlog-never-settled');
	}
	return anomalies;
}
