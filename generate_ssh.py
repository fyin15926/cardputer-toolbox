import subprocess
import os

# Generate SSH key
key_path = os.path.join(os.environ['USERPROFILE'], '.ssh', 'id_rsa')
ssh_keygen = r"C:\Program Files\Git\usr\bin\ssh-keygen.exe"

proc = subprocess.Popen(
    [ssh_keygen, '-t', 'rsa', '-b', '4096', '-C', 'fyin15926@github', '-f', key_path, '-N', ''],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    shell=False
)

stdout, stderr = proc.communicate()
print(stdout.decode('utf-8', errors='ignore'))
print(stderr.decode('utf-8', errors='ignore'))
