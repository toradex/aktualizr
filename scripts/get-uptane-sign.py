#!/usr/bin/env python3

import argparse
import hashlib
import json
import os.path
import requests
import shutil
import sys
import tarfile
import urllib.request
import xml.etree.ElementTree as ET

from pathlib import Path


github_api = 'https://api.github.com/repos/uptane/ota-tuf/releases'


def main():
    parser = argparse.ArgumentParser(description='Download a specific or the latest version of uptane-sign')
    parser.add_argument('-a', '--archive', help='static local archive')
    parser.add_argument('-n', '--name', help='specific version to download')
    parser.add_argument('-s', '--sha256', help='expected hash of requested version')
    parser.add_argument('-o', '--output', type=Path, default=Path('.'), help='download directory')
    args = parser.parse_args()

    if not args.output.exists():
        print('Error: specified output directory ' + args.output + ' does not exist!')
        return 1

    if args.archive:
        path = args.archive
    else:
        path = find_version(args.name, args.sha256, args.output)
        if path is None:
            return 1

    # Remove anything leftover inside the extracted directory.
    for extracted in ["uptane-sign", "garage-sign"]:
        extract_path = args.output.joinpath(extracted)
        if extract_path.exists():
           shutil.rmtree(str(extract_path))
    # Always extract everything.
    t = tarfile.open(str(path))
    t.extractall(path=str(args.output))
    return 0


def find_version(version_name, sha256_hash, output):
    if sha256_hash and not version_name:
        print('Warning: sha256 hash specified without specifying a version.')
    if version_name and not sha256_hash:
        print('Warning: specific version requested without specifying the sha256 hash.')

    r = requests.get(
        f'{github_api}',
        headers = {
            'Accept': 'application/vnd.github+json',
            'X-GitHub-Api-Version': '2022-11-28'
        }
    )
    if r.status_code != 200:
        print('Error: unable to request index!')
        return None

    versions = {tag['name']: tag['assets'][0]['size'] for tag in r.json()}
    urls = {tag['name']: tag['assets'][0]['browser_download_url'] for tag in r.json()}
    if version_name:
        name = version_name
        if name not in versions:
            print('Error: ' + name + ' not found in tuf-cli releases.')
            return None
    else:
        name = list(versions.keys())[0]

    size = versions[name]
    url = urls[name]
    path = output.joinpath(name + '.tgz')
    if not path.is_file() or not verify(path, size, sha256_hash):
        print('Downloading ' + name + ' from server...')
        if download(url, path, size, sha256_hash):
            print(name + ' successfully downloaded and validated.')
            return path
        else:
            return None
    print(name + ' already present and validated.')
    return path


def download(url, path, size, sha256_hash):
    r = urllib.request.urlopen(url)
    if r.status != 200:
        print('Error: unable to request file!')
        return False
    with path.open(mode='wb') as f:
        shutil.copyfileobj(r, f)
    return verify(path, size, sha256_hash)


def verify(path, size, sha256_hash):
    if not tarfile.is_tarfile(str(path)):
        print('Error: ' + os.path.basename(path) + ' is not a valid tar archive!')
        return False
    actual_size = os.path.getsize(str(path))
    if actual_size != int(size):
        print('Error: size of ' + os.path.basename(path) + ' (' + str(actual_size) + ') does not match expected value (' + str(size) + ')!')
        return False
    if sha256_hash:
        s = hashlib.sha256()
        with path.open(mode='rb') as f:
            data = f.read()
            s.update(data)
        if s.hexdigest() != sha256_hash:
            print('Error: sha256 hash of ' + os.path.basename(path) + ' does not match provided value!')
            return False
    return True


if __name__ == '__main__':
    sys.exit(main())
