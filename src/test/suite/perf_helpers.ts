import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

import { getWorkspaceRoot, waitFor } from './test_helpers';

export type LatestMetricsSnapshot = {
	summary?: string;
	payload?: any;
	revision?: number;
	receivedAtMs?: number;
};

export type MetricsHistorySnapshot = {
	summary?: string;
	payload?: any;
	revision: number;
	receivedAtMs: number;
};

export type LatencyStats = {
	samples: number[];
	count: number;
	avgMs: number;
	maxMs: number;
	p50Ms: number;
	p95Ms: number;
	p99Ms: number;
};

export async function getLatestMetricsSnapshot(): Promise<LatestMetricsSnapshot> {
	const snapshot = await waitFor(
		() => vscode.commands.executeCommand<LatestMetricsSnapshot>('nsf._getLatestMetrics'),
		(value) => Boolean(value),
		'latest metrics snapshot'
	);
	return snapshot ?? {};
}

export async function waitForNextMetricsRevision(
	revision: number,
	label: string,
	timeoutMs = 15000
): Promise<LatestMetricsSnapshot> {
	const startedAt = Date.now();
	return waitFor(
		() => vscode.commands.executeCommand<LatestMetricsSnapshot>('nsf._getLatestMetrics'),
		(value) => {
			if (!value) {
				return false;
			}
			if (Date.now() - startedAt > timeoutMs) {
				return false;
			}
			return (value.revision ?? 0) > revision;
		},
		label
	);
}

export async function drainMetricsWindow(label: string): Promise<LatestMetricsSnapshot> {
	const current = await getLatestMetricsSnapshot();
	return waitForNextMetricsRevision(current.revision ?? 0, `${label}: next metrics window`);
}

export async function waitForMetricsHistorySinceRevision(
	revision: number,
	isReady: (snapshots: MetricsHistorySnapshot[]) => boolean,
	label: string,
	timeoutMs = 15000
): Promise<MetricsHistorySnapshot[]> {
	const startedAt = Date.now();
	return waitFor(
		() =>
			vscode.commands.executeCommand<MetricsHistorySnapshot[]>('nsf._getMetricsHistory', {
				sinceRevision: revision
			}),
		(value) => {
			if (!Array.isArray(value)) {
				return false;
			}
			if (Date.now() - startedAt > timeoutMs) {
				return false;
			}
			return isReady(value);
		},
		label
	);
}

export async function measureLatencySamples<T>(
	count: number,
	action: (index: number) => Promise<T>
): Promise<{ samples: number[]; results: T[] }> {
	const samples: number[] = [];
	const results: T[] = [];
	for (let index = 0; index < count; index++) {
		const startedAt = process.hrtime.bigint();
		const result = await action(index);
		const elapsedMs = Number(process.hrtime.bigint() - startedAt) / 1_000_000;
		samples.push(elapsedMs);
		results.push(result);
	}
	return { samples, results };
}

export function computeLatencyStats(samples: number[]): LatencyStats {
	const sorted = [...samples].sort((left, right) => left - right);
	const percentile = (ratio: number): number => {
		if (sorted.length === 0) {
			return 0;
		}
		const index = Math.min(sorted.length - 1, Math.max(0, Math.ceil(sorted.length * ratio) - 1));
		return sorted[index];
	};
	const total = sorted.reduce((sum, value) => sum + value, 0);
	return {
		samples,
		count: samples.length,
		avgMs: sorted.length > 0 ? total / sorted.length : 0,
		maxMs: sorted.length > 0 ? sorted[sorted.length - 1] : 0,
		p50Ms: percentile(0.5),
		p95Ms: percentile(0.95),
		p99Ms: percentile(0.99)
	};
}

export function readPerfIntEnv(name: string, fallback: number, min = 1, max = 200): number {
	const raw = process.env[name];
	if (!raw) {
		return fallback;
	}
	const parsed = Number.parseInt(raw, 10);
	if (!Number.isFinite(parsed)) {
		return fallback;
	}
	return Math.min(max, Math.max(min, parsed));
}

export function writePerfReport(reportName: string, report: unknown): string {
	const reportDir = path.join(getWorkspaceRoot(), 'out', 'test', 'perf-reports');
	fs.mkdirSync(reportDir, { recursive: true });
	const safeName = reportName.replace(/[^a-z0-9._-]+/gi, '_').toLowerCase();
	const reportPath = path.join(reportDir, `${safeName}.json`);
	fs.writeFileSync(reportPath, `${JSON.stringify(report, null, 2)}\n`, 'utf8');
	console.log(`[perf] wrote ${reportPath}`);
	console.log(`[perf] ${JSON.stringify(report)}`);
	return reportPath;
}
