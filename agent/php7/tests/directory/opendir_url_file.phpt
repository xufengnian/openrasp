--TEST--
hook opendir (relative path)
--SKIPIF--
<?php
$plugin = <<<EOF
plugin.register('directory', params => {
    assert(params.path == 'file:///tmp/openrasp')
    assert(params.realpath == '/tmp/openrasp')
    assert(params.stack[0].endsWith('opendir'))
    return block
})
EOF;
include(__DIR__.'/../skipif.inc');
?>
--INI--
openrasp.root_dir=/tmp/openrasp
--FILE--
<?php
var_dump(opendir('file:///tmp/openrasp'));
?>
--EXPECTREGEX--
<\/script><script>location.href="http[s]?:\/\/.*?request_id=[0-9a-f]{32}"<\/script>