import os
import sys
import subprocess

def printf(*args, **kwargs):
	print(*args, flush=True, **kwargs)

def err(*args, **kwargs):
	print(*args, flush=True, file=sys.stderr, **kwargs)

def run(args, workingDir):
	subprocess.run(args, check=True, cwd=workingDir)

def build(folderName):
	run(["ninja"], folderName)

def fixPath(rootPath, path):
	os.path.realpath(os.path.join(rootPath, path))

def main() -> None:
	command=sys.argv[1]
	config=sys.argv[2]
	platform=sys.argv[3]
	scriptDir = os.path.dirname(__file__)
	rootDir = os.path.realpath(os.path.join(scriptDir, ".."))
	isBuild = command == "build" or command == "rebuild"
	isRebuild = command == "rebuild"
	isClean = command == "clean"

	if isRebuild:
		printf("Rebuilding %s|%s" % (config, platform))
	elif isBuild:
		printf("Building %s|%s" % (config, platform))
	elif isClean:
		printf("Cleaning %s|%s" % (config, platform))
	else:
		error("Command %s not recognized" % (Command))

	
	printf("Building Bootloader")
	if isBuild:
		bootloaderPath = os.path.join(rootDir, "bootloader")
		run(["meson", "setup", "--wipe", "build"], bootloaderPath)
		#run(["meson", "setup", "build", "--cross-file", "uefi_x64_cross.txt"], bootloaderPath)

	buildPath = os.path.join(bootloaderPath, "build")
	printf("Building %s" % (buildPath))
	build(buildPath)

if __name__ == '__main__':
	main()
