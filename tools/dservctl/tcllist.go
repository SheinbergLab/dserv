package main

// ParseTclList parses a Tcl list string into individual elements.
// Handles brace-grouping {word with spaces}, double-quoted "word", and bare words.
func ParseTclList(input string) []string {
	var result []string
	input = trimSpace(input)
	i := 0
	for i < len(input) {
		// Skip whitespace
		for i < len(input) && (input[i] == ' ' || input[i] == '\t') {
			i++
		}
		if i >= len(input) {
			break
		}

		var elem string
		switch input[i] {
		case '{':
			elem, i = parseBraced(input, i)
		case '"':
			elem, i = parseQuoted(input, i)
		default:
			elem, i = parseBare(input, i)
		}
		result = append(result, elem)
	}
	return result
}

// parseBraced extracts a brace-grouped element: {content}
func parseBraced(s string, start int) (string, int) {
	i := start + 1 // skip opening brace
	depth := 1
	var buf []byte
	for i < len(s) && depth > 0 {
		if s[i] == '{' {
			depth++
			buf = append(buf, s[i])
		} else if s[i] == '}' {
			depth--
			if depth > 0 {
				buf = append(buf, s[i])
			}
		} else if s[i] == '\\' && i+1 < len(s) {
			// In braces, only \newline is special; pass everything else literally
			buf = append(buf, s[i])
			i++
			buf = append(buf, s[i])
		} else {
			buf = append(buf, s[i])
		}
		i++
	}
	return string(buf), i
}

// parseQuoted extracts a double-quoted element: "content"
func parseQuoted(s string, start int) (string, int) {
	i := start + 1 // skip opening quote
	var buf []byte
	for i < len(s) {
		if s[i] == '"' {
			i++ // skip closing quote
			return string(buf), i
		}
		if s[i] == '\\' && i+1 < len(s) {
			i++
			switch s[i] {
			case 'n':
				buf = append(buf, '\n')
			case 't':
				buf = append(buf, '\t')
			case '\\':
				buf = append(buf, '\\')
			case '"':
				buf = append(buf, '"')
			default:
				buf = append(buf, '\\', s[i])
			}
		} else {
			buf = append(buf, s[i])
		}
		i++
	}
	return string(buf), i
}

// parseBare extracts a bare word (whitespace-delimited)
func parseBare(s string, start int) (string, int) {
	i := start
	var buf []byte
	for i < len(s) && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' {
		if s[i] == '\\' && i+1 < len(s) {
			i++
			buf = append(buf, s[i])
		} else {
			buf = append(buf, s[i])
		}
		i++
	}
	return string(buf), i
}

func trimSpace(s string) string {
	start := 0
	for start < len(s) && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r') {
		start++
	}
	end := len(s)
	for end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\n' || s[end-1] == '\r') {
		end--
	}
	return s[start:end]
}
