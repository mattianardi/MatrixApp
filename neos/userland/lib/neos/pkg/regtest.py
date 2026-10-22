import subprocess, os
r = subprocess.run(["/bin/neosctl", "pkg.record", "x", "1", "y"], capture_output=True, text=True)
print("rc", r.returncode, repr(r.stdout), repr(r.stderr))
print("pid", os.getpid())
