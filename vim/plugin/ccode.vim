if exists('g:loaded_ccode')
	finish
endif
let g:loaded_ccode = 1

au FileType c,cpp,objc,objcpp call s:ccodeInit()

fu! s:ccodeCurrentBuffer()
	let buf = getline(1, '$')
	let file = tempname()
	call writefile(buf, file)
	return file
endf

fu! s:system(str, ...)
	return (a:0 == 0 ? system(a:str) : system(a:str, join(a:000)))
endf

fu! s:ccodeCommand(cmd, args)
	for i in range(0, len(a:args) - 1)
		let a:args[i] = shellescape(a:args[i])
	endfor
	let cmdstr = printf('ccode %s %s', a:cmd, join(a:args))
	let result = s:system(cmdstr)
	if v:shell_error != 0
		return "[\"0\", []]"
	else
		return result
	endif
endf

fu! s:ccodeCurBufOpt(filename)
	return '-in=' . a:filename
endf

fu! s:ccodeLine()
	return printf('%d', line('.'))
endf

fu! s:ccodeCol()
	return printf('%d', col('.'))
endf

fu! s:ccodeAutocomplete()
	let filename = s:ccodeCurrentBuffer()
	let result = s:ccodeCommand('ac', [s:ccodeCurBufOpt(filename), bufname('%'),
				   \ s:ccodeLine(), s:ccodeCol()])
	call delete(filename)
	return result
endf

fu! CCodeComplete(findstart, base)
	"findstart = 1 when we need to get the text length
	if a:findstart == 1
		execute "silent let g:ccode_completions = " . s:ccodeAutocomplete()
		return col('.') - g:ccode_completions[0] - 1
	"findstart = 0 when we need to return the list of completions
	else
		return g:ccode_completions[1]
	endif
endf

fu! s:ccodeInit()
	setlocal omnifunc=CCodeComplete
endf
