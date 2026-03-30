import * as fs from 'fs';
import * as path from 'path';
import Mocha = require('mocha');

export async function run(): Promise<void> {
	const mocha = new Mocha({
		ui: 'bdd',
		color: true,
		timeout: 60000
	});

	const suiteRoot = __dirname;
	const sourceSuiteRoot = path.resolve(suiteRoot, '..', '..', '..', 'src', 'test', 'suite');
	const fileFilter = (process.env.NSF_TEST_FILE_FILTER ?? '').trim().toLowerCase();
	const testFiles: string[] = [];
	for (const file of fs.readdirSync(suiteRoot)) {
		if (!file.endsWith('.test.js')) {
			continue;
		}
		if (fileFilter && !file.toLowerCase().includes(fileFilter)) {
			continue;
		}
		const sourceFile = path.join(sourceSuiteRoot, file.replace(/\.js$/, '.ts'));
		if (!fs.existsSync(sourceFile)) {
			continue;
		}
		testFiles.push(path.join(suiteRoot, file));
		mocha.addFile(path.join(suiteRoot, file));
	}

	console.log(`[suite] loading ${testFiles.length} test file(s)`);

	await new Promise<void>((resolve, reject) => {
		mocha.run((failures) => {
			console.log(`[suite] completed with ${failures} failure(s)`);
			if (failures > 0) {
				reject(new Error(`${failures} test(s) failed.`));
				return;
			}
			resolve();
		});
	});
}
