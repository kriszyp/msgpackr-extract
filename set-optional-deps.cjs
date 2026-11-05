let fs = require('fs');
packageData = JSON.parse(fs.readFileSync('package.json'));
let prebuilds = fs.readdirSync('prebuilds');
// without a binary for its libc, an install falls back to compiling from source
for (let linux of ['linux-x64', 'linux-arm64', 'linux-arm']) {
	for (let libc of ['glibc', 'musl']) {
		if (!fs.existsSync('prebuilds/' + linux + '/node.napi.' + libc + '.node'))
			throw new Error('prebuilds/' + linux + ' has no ' + libc + ' binary; check that every prebuild job uploaded to the release');
	}
}
let platformDeps = packageData.optionalDependencies = {};
let packageName = packageData.name;
let version = packageData.version;
for (let prebuild of prebuilds) {
	platformDeps['@' + packageName + '/' + packageName + '-' + prebuild] = version;
}
fs.writeFileSync('package.json', JSON.stringify(packageData, null, 2));
