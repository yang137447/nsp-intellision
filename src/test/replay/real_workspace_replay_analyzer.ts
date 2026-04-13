import type { ReplaySampleSnapshot, ReplayStep } from './real_workspace_replay_types';

export function detectReplayAnomalies(step: ReplayStep, samples: ReplaySampleSnapshot[]): string[] {
	const anomalies: string[] = [];
	if (samples.length === 0) {
		return anomalies;
	}

	const windowLabel = step.samplingWindow?.label.toLowerCase() ?? '';

	const counterObserved = (field: string) => {
		const baselineCount = samples[0].baselineInternalStatus?.[field];
		if (typeof baselineCount === 'number') {
			return samples.some((sample) => {
				const current = sample.internalStatus?.[field];
				return typeof current === 'number' && current > baselineCount;
			});
		}

		const firstCount = samples[0].internalStatus?.[field];
		if (typeof firstCount === 'number' && firstCount > 0) {
			return true;
		}
		if (samples.length <= 1) {
			return true;
		}
		for (let index = 1; index < samples.length; index++) {
			const previous = samples[index - 1].internalStatus?.[field];
			const current = samples[index].internalStatus?.[field];
			if (typeof previous === 'number' && typeof current === 'number' && current > previous) {
				return true;
			}
		}
		return false;
	};

	if (windowLabel.includes('completion') || windowLabel.includes('member')) {
		if (!counterObserved('completionRequestCount')) {
			anomalies.push('completion-request-not-observed');
		}
	}
	if (windowLabel.includes('signature') || (step.kind === 'typeText' && step.payload.text.includes('('))) {
		if (!counterObserved('signatureHelpRequestCount')) {
			anomalies.push('signature-help-request-not-observed');
		}
	}
	if (samples.every((sample) => (sample.internalStatus?.activeRpcCount ?? 0) > 0)) {
		anomalies.push('active-rpc-backlog-never-settled');
	}
	return anomalies;
}
