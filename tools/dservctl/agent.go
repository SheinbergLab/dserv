package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

// AgentClient handles HTTP communication with the dserv-agent REST API.
type AgentClient struct {
	BaseURL    string
	Token      string
	HTTPClient *http.Client
}

// NewAgentClient creates an AgentClient from config.
func NewAgentClient(cfg *Config) *AgentClient {
	return &AgentClient{
		BaseURL: cfg.AgentBaseURL(),
		Token:   cfg.Token,
		HTTPClient: &http.Client{
			Timeout: 30 * time.Second,
		},
	}
}

// Do performs an HTTP request with auth and returns parsed JSON.
func (c *AgentClient) Do(method, path string, body interface{}) (map[string]interface{}, error) {
	url := c.BaseURL + path

	var bodyReader io.Reader
	if body != nil {
		data, err := json.Marshal(body)
		if err != nil {
			return nil, fmt.Errorf("marshal request body: %w", err)
		}
		bodyReader = bytes.NewReader(data)
	}

	req, err := http.NewRequest(method, url, bodyReader)
	if err != nil {
		return nil, fmt.Errorf("create request: %w", err)
	}

	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+c.Token)
	}

	resp, err := c.HTTPClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("request to %s failed: %w", path, err)
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("read response: %w", err)
	}

	var result map[string]interface{}
	if len(respBody) > 0 {
		if err := json.Unmarshal(respBody, &result); err != nil {
			// Try as array — some endpoints return arrays
			var arr []interface{}
			if err2 := json.Unmarshal(respBody, &arr); err2 == nil {
				result = map[string]interface{}{"items": arr}
			} else {
				// Return raw text
				result = map[string]interface{}{"raw": string(respBody)}
			}
		}
	}

	if resp.StatusCode >= 400 {
		errMsg := fmt.Sprintf("HTTP %d", resp.StatusCode)
		if result != nil {
			if e, ok := result["error"].(string); ok {
				errMsg = e
			}
		}
		return result, fmt.Errorf("%s", errMsg)
	}

	return result, nil
}

// Get performs a GET request.
func (c *AgentClient) Get(path string) (map[string]interface{}, error) {
	return c.Do("GET", path, nil)
}

// Post performs a POST request with a JSON body.
func (c *AgentClient) Post(path string, body interface{}) (map[string]interface{}, error) {
	return c.Do("POST", path, body)
}

// GetRaw performs a GET request and returns the raw response bytes.
// Useful for endpoints that return non-JSON (e.g., ZIP files).
func (c *AgentClient) GetRaw(path string) ([]byte, string, error) {
	url := c.BaseURL + path

	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, "", fmt.Errorf("create request: %w", err)
	}
	if c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+c.Token)
	}

	resp, err := c.HTTPClient.Do(req)
	if err != nil {
		return nil, "", fmt.Errorf("request to %s failed: %w", path, err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, "", fmt.Errorf("read response: %w", err)
	}

	if resp.StatusCode >= 400 {
		return nil, "", fmt.Errorf("HTTP %d: %s", resp.StatusCode, string(body))
	}

	return body, resp.Header.Get("Content-Type"), nil
}
