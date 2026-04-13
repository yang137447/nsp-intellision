import * as fs from 'fs';
import * as path from 'path';

export function writeReplayReport(reportName: string, report: unknown): string {
	const repoRoot = path.resolve(__dirname, '..', '..', '..');
	const reportDir = path.join(repoRoot, 'out', 'test', 'perf-reports', 'real-replay');
	fs.mkdirSync(reportDir, { recursive: true });
	const safeName = reportName.replace(/[^a-z0-9._-]+/gi, '_').toLowerCase();
	const reportPath = path.join(reportDir, `${safeName}.json`);
	fs.writeFileSync(reportPath, `${JSON.stringify(report, null, 2)}\n`, 'utf8');
	console.log(`[real-replay] wrote ${reportPath}`);
	return reportPath;
}
