import os
import urllib2
import multiprocessing as mp
import neblio_ci_libs as nci

working_dir = os.getcwd()
deploy_dir = os.path.join(os.environ['TRAVIS_BUILD_DIR'],'deploy', '')

# If this is a PR, bail out instead of just wasting 45 mins running
if (os.environ['TRAVIS_PULL_REQUEST'] != 'false'):
  print('Pull Requests are not built for RPi since ccache cannot be used!')
  exit(0)

nci.mkdir_p(deploy_dir)
os.chdir(deploy_dir)

build_target = ''
build_target_alt = ''
if(os.environ['target_v'] == "rpi_daemon"):
  build_target = 'nebliod'
  build_target_alt = 'nebliod'
else:
  build_target = 'neblio-qt'
  build_target_alt = 'neblio-Qt'

# Install docker
nci.call_with_err_code('curl -fsSL https://get.docker.com -o get-docker.sh && sudo sh get-docker.sh && rm get-docker.sh')

# Prepare qemu
nci.call_with_err_code('docker run --rm --privileged multiarch/qemu-user-static:register --reset')

# move .ccache folder to our deploy directory
nci.call_with_err_code('mv ' + os.path.join(os.environ['HOME'],'.ccache', '') + ' ' + os.path.join(deploy_dir,'.ccache', ''))

# Start Docker Container to Build nebliod or neblio-Qt
nci.call_with_err_code('timeout --signal=SIGKILL 42m sudo docker run -e BRANCH=' + os.environ['TRAVIS_BRANCH'] + ' -e BUILD=' + build_target + ' -v ' + os.environ['TRAVIS_BUILD_DIR'] + ':/root/vol -t neblioteam/nebliod-build-ccache-rpi')
nci.call_with_err_code('sleep 15 && sudo docker kill $(sudo docker ps -q);exit 0')

# move .ccache folder back to travis ccache dir
nci.call_with_err_code('mv ' + os.path.join(deploy_dir,'.ccache', '') + ' ' + os.path.join(os.environ['HOME'],'.ccache', ''))

file_name = '$(date +%Y-%m-%d)---' + os.environ['TRAVIS_BRANCH'] + '-' + os.environ['TRAVIS_COMMIT'][:7] + '---' + build_target_alt + '---RPi-raspbian-stretch.tar.gz'

#Removed extra build settings as we are not posting anything to GitHub releases
