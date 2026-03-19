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
	const testFiles: string[] = [];
	for (const file of fs.readdirSync(suiteRoot)) {
		if (!file.endsWith('.test.js')) {
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
