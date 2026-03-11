package main

import (
	"reflect"
	"testing"
)

func TestParseTclListBareWords(t *testing.T) {
	tests := []struct {
		input string
		want  []string
	}{
		{"", nil},
		{"hello", []string{"hello"}},
		{"hello world", []string{"hello", "world"}},
		{"  hello   world  ", []string{"hello", "world"}},
		{"one two three", []string{"one", "two", "three"}},
	}
	for _, tt := range tests {
		got := ParseTclList(tt.input)
		if !reflect.DeepEqual(got, tt.want) {
			t.Errorf("ParseTclList(%q) = %v, want %v", tt.input, got, tt.want)
		}
	}
}

func TestParseTclListBraced(t *testing.T) {
	tests := []struct {
		input string
		want  []string
	}{
		{"{hello world}", []string{"hello world"}},
		{"{hello} {world}", []string{"hello", "world"}},
		{"{info commands} {info complete} {info coroutine}", []string{"info commands", "info complete", "info coroutine"}},
		{"foo {bar baz} qux", []string{"foo", "bar baz", "qux"}},
		{"{nested {braces}}", []string{"nested {braces}"}},
	}
	for _, tt := range tests {
		got := ParseTclList(tt.input)
		if !reflect.DeepEqual(got, tt.want) {
			t.Errorf("ParseTclList(%q) = %v, want %v", tt.input, got, tt.want)
		}
	}
}

func TestParseTclListQuoted(t *testing.T) {
	tests := []struct {
		input string
		want  []string
	}{
		{`"hello world"`, []string{"hello world"}},
		{`"hello" "world"`, []string{"hello", "world"}},
		{`"escaped \"quote\""`, []string{`escaped "quote"`}},
		{`"tab\there"`, []string{"tab\there"}},
	}
	for _, tt := range tests {
		got := ParseTclList(tt.input)
		if !reflect.DeepEqual(got, tt.want) {
			t.Errorf("ParseTclList(%q) = %v, want %v", tt.input, got, tt.want)
		}
	}
}

func TestParseTclListMixed(t *testing.T) {
	tests := []struct {
		input string
		want  []string
	}{
		{`foo {bar baz} "hello world" qux`, []string{"foo", "bar baz", "hello world", "qux"}},
		{`commands complete coroutine`, []string{"commands", "complete", "coroutine"}},
	}
	for _, tt := range tests {
		got := ParseTclList(tt.input)
		if !reflect.DeepEqual(got, tt.want) {
			t.Errorf("ParseTclList(%q) = %v, want %v", tt.input, got, tt.want)
		}
	}
}

func TestParseTclListCompletionResponse(t *testing.T) {
	// Typical completion responses from complete_token
	tests := []struct {
		input string
		want  []string
	}{
		// Simple word list (most common for complete_token)
		{"commands complete coroutine", []string{"commands", "complete", "coroutine"}},
		// Braced list (from complete command)
		{"{set tcl_platform} {set tcl_patchLevel}", []string{"set tcl_platform", "set tcl_patchLevel"}},
		// Single match
		{"hostname", []string{"hostname"}},
		// Empty
		{"", nil},
	}
	for _, tt := range tests {
		got := ParseTclList(tt.input)
		if !reflect.DeepEqual(got, tt.want) {
			t.Errorf("ParseTclList(%q) = %v, want %v", tt.input, got, tt.want)
		}
	}
}
