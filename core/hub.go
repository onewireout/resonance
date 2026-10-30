package main

import (
	"bufio"
	"bytes"
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
)

var indexHTML string
var modelsConfigPath string
var tlsCertPath string
var tlsKeyPath string

type hubState struct {
	mu           sync.RWMutex
	model        string
	systemPrompt string
}

var state = &hubState{
	model: DefaultModel,
}

type modelEntry struct {
	SystemPrompt string `json:"system_prompt"`
}

type modelsConfig struct {
	Models      map[string]modelEntry `json:"models"`
	ActiveModel string                `json:"active_model"`
}

func loadModelsConfig() {
	data, err := os.ReadFile(modelsConfigPath)
	if err != nil {
		log.Printf("Models config not found, using current state: %v", err)
		return
	}
	var cfg modelsConfig
	if err := json.Unmarshal(data, &cfg); err != nil {
		log.Printf("Failed to parse models config: %v", err)
		return
	}
	state.mu.Lock()
	defer state.mu.Unlock()
	if cfg.ActiveModel != "" {
		state.model = cfg.ActiveModel
	}
	if entry, ok := cfg.Models[state.model]; ok {
		state.systemPrompt = entry.SystemPrompt
	} else {
		state.systemPrompt = ""
	}
	log.Printf("Reloaded config: model=%s", state.model)
}

const (
	HubSocketPath   = "/tmp/resonance-hub.sock"
	BrainSocketPath = "/tmp/resonance.sock"
	LlamaAPIURL     = "http://localhost:11434/v1/chat/completions"
	DefaultModel    = "qwen3.8:27B-4bit-Smaller"
	WebUIPort       = ":8080"
)

// Message matches llama-server's OpenAI-compatible message format and represents a chat message.
type Message struct {
	Role     string `json:"role"`
	Content  string `json:"content"`
	Thinking string `json:"thinking,omitempty"`
}

// Request is the JSON received from a client source.
type Request struct {
	Prompt          string    `json:"prompt,omitempty"`
	Bypass          bool      `json:"bypass,omitempty"`
	IncludeThinking bool      `json:"include_thinking,omitempty"`
	Messages        []Message `json:"messages,omitempty"`
	Model           string    `json:"model,omitempty"`
}

// ResponseChunk is the JSON streamed back to the client.
type ResponseChunk struct {
	Token    string `json:"token,omitempty"`
	Thinking string `json:"thinking,omitempty"`
	Done     bool   `json:"done"`
	Error    string `json:"error,omitempty"`
}

// ChatCompletionRequest is the payload sent to llama-server's OpenAI-compatible API.
type ChatCompletionRequest struct {
	Model              string         `json:"model"`
	Messages           []Message      `json:"messages"`
	Stream             bool           `json:"stream"`
	ChatTemplateKwargs map[string]any `json:"chat_template_kwargs,omitempty"`
}

// ChatCompletionChunk is a single SSE chunk streamed back from llama-server.
type ChatCompletionChunk struct {
	Choices []struct {
		Delta struct {
			Content          string `json:"content"`
			ReasoningContent string `json:"reasoning_content"`
		} `json:"delta"`
		FinishReason *string `json:"finish_reason"`
	} `json:"choices"`
}

func main() {
	// Clean up any stale socket from a previous run
	if err := os.RemoveAll(HubSocketPath); err != nil {
		log.Fatalf("Failed to remove stale socket: %v", err)
	}

	// Listen on Unix domain socket
	listener, err := net.Listen("unix", HubSocketPath)
	if err != nil {
		log.Fatalf("Failed to listen on socket %s: %v", HubSocketPath, err)
	}
	defer listener.Close()
	defer os.Remove(HubSocketPath)

	log.Printf("Resonance LLM Hub listening on %s", HubSocketPath)

	// Start the HTTP WebUI server on all interfaces
	go func() {
		mux := http.NewServeMux()
		mux.HandleFunc("/", handleIndex)
		mux.HandleFunc("/chat", handleChatHTTP)
		mux.HandleFunc("/api/model", handleGetModel)
		log.Printf("Resonance WebUI listening on %s (TLS)", WebUIPort)
		if err := http.ListenAndServeTLS(WebUIPort, tlsCertPath, tlsKeyPath, mux); err != nil {
			log.Fatalf("HTTPS server error: %v", err)
		}
	}()

	// Clean up socket file on Ctrl+C or system termination
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigChan
		log.Println("Shutting down... cleaning up socket")
		listener.Close()
		os.Remove(HubSocketPath)
		os.Exit(0)
	}()

	// Accept connections loop
	for {
		conn, err := listener.Accept()
		if err != nil {
			select {
			case <-sigChan:
				return // Shutting down
			default:
				log.Printf("Accept error: %v", err)
				continue
			}
		}

		// Handle connection in a background goroutine
		go handleConnection(conn)
	}
}

// handleIndex serves the embedded WebUI page.
func handleIndex(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	fmt.Fprint(w, indexHTML)
}

// handleGetModel returns the current active model name.
func handleGetModel(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	state.mu.RLock()
	model := state.model
	state.mu.RUnlock()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"model": model})
}

// handleChatHTTP accepts a POST request with a JSON body, streams llama.cpp tokens
// back to the browser as Server-Sent Events.
func handleChatHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodOptions {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req Request
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, fmt.Sprintf("invalid request: %v", err), http.StatusBadRequest)
		return
	}

	state.mu.RLock()
	activeModel := req.Model
	if activeModel == "" {
		activeModel = state.model
	}
	systemPrompt := state.systemPrompt
	state.mu.RUnlock()

	var messages []Message
	if systemPrompt != "" {
		messages = append(messages, Message{Role: "system", Content: systemPrompt})
	}
	if len(req.Messages) > 0 {
		messages = append(messages, req.Messages...)
	} else if req.Prompt != "" {
		messages = append(messages, Message{Role: "user", Content: req.Prompt})
	} else {
		http.Error(w, "empty prompt and message list", http.StatusBadRequest)
		return
	}

	// SSE headers
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming not supported", http.StatusInternalServerError)
		return
	}

	streamLlamaSSE(w, flusher, messages, activeModel, req.IncludeThinking)
}

func init() {
	// 1. Get the absolute path to the directory where this hub binary actually lives (the core folder)
	exePath, err := os.Executable()
	if err != nil {
		fmt.Println("[ERROR] Cannot find executable path:", err)
		return
	}

	// 2. Step up one level from 'core' to get to the main 'Resonance' project root
	projectRoot := filepath.Join(filepath.Dir(exePath), "..")

	// 3. Build the absolute path straight to your organized HTML file
	htmlPath := filepath.Join(projectRoot, "tools/webui/index.html")

	// 4. Read the file using the absolute path
	bytes, err := os.ReadFile(htmlPath)
	if err != nil {
		fmt.Println("[ERROR] Failed to load HTML file at:", htmlPath, err)
	} else {
		indexHTML = string(bytes)
	}

	modelsConfigPath = filepath.Join(projectRoot, "tools/tuner/models.json")
	loadModelsConfig()

	tlsCertPath = filepath.Join(projectRoot, "core/tls/hub.crt")
	tlsKeyPath = filepath.Join(projectRoot, "core/tls/hub.key")
}

// streamLlamaSSE calls llama-server's API and writes tokens as SSE events to the HTTP response.
func streamLlamaSSE(w http.ResponseWriter, flusher http.Flusher, messages []Message, model string, includeThinking bool) {
	if model == "" {
		model = DefaultModel
	}

	chatReq := ChatCompletionRequest{
		Model:    model,
		Messages: messages,
		Stream:   true,
	}
	if !includeThinking {
		chatReq.ChatTemplateKwargs = map[string]any{"enable_thinking": false}
	}

	payloadBytes, err := json.Marshal(chatReq)
	if err != nil {
		sendSSEError(w, flusher, fmt.Sprintf("failed to serialize llama.cpp request: %v", err))
		return
	}

	resp, err := http.Post(LlamaAPIURL, "application/json", bytes.NewBuffer(payloadBytes))
	if err != nil {
		sendSSEError(w, flusher, fmt.Sprintf("failed to contact llama.cpp service: %v. Is llama.cpp running?", err))
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		msg := strings.TrimSpace(string(body))
		if msg == "" {
			msg = fmt.Sprintf("llama.cpp returned status code %d", resp.StatusCode)
		}
		sendSSEError(w, flusher, msg)
		return
	}

	scanner := bufio.NewScanner(resp.Body)
	for scanner.Scan() {
		payload, ok := parseSSELine(scanner.Text())
		if !ok {
			continue
		}

		if payload == "[DONE]" {
			data, _ := json.Marshal(ResponseChunk{Done: true})
			fmt.Fprintf(w, "data: %s\n\n", data)
			flusher.Flush()
			break
		}

		var chunk ChatCompletionChunk
		if err := json.Unmarshal([]byte(payload), &chunk); err != nil {
			log.Printf("Failed to unmarshal llama.cpp chunk: %v", err)
			continue
		}
		if len(chunk.Choices) == 0 {
			continue
		}

		token := chunk.Choices[0].Delta.Content
		var thinking string
		if includeThinking {
			thinking = chunk.Choices[0].Delta.ReasoningContent
		}

		if token == "" && thinking == "" {
			continue
		}

		clientChunk := ResponseChunk{
			Token:    token,
			Thinking: thinking,
		}

		data, err := json.Marshal(clientChunk)
		if err != nil {
			continue
		}

		fmt.Fprintf(w, "data: %s\n\n", data)
		flusher.Flush()
	}

	if err := scanner.Err(); err != nil {
		sendSSEError(w, flusher, fmt.Sprintf("error reading stream from llama.cpp: %v", err))
	}
}

// parseSSELine strips the "data: " prefix from an SSE line, reporting whether the line was a data line.
func parseSSELine(line string) (string, bool) {
	if !strings.HasPrefix(line, "data: ") {
		return "", false
	}
	return strings.TrimPrefix(line, "data: "), true
}

// sendSSEError writes an error as a final SSE event.
func sendSSEError(w http.ResponseWriter, flusher http.Flusher, msg string) {
	chunk := ResponseChunk{Error: msg, Done: true}
	data, _ := json.Marshal(chunk)
	fmt.Fprintf(w, "data: %s\n\n", data)
	flusher.Flush()
}

// ControlRequest is a command to update hub state rather than send a chat message.
type ControlRequest struct {
	SetModel        string `json:"set_model,omitempty"`
	SetSystemPrompt string `json:"set_system_prompt,omitempty"`
	Reload          bool   `json:"reload,omitempty"`
}

// handleConnection handles an active client session.
func handleConnection(conn net.Conn) {
	defer conn.Close()

	var raw json.RawMessage
	dec := json.NewDecoder(conn)
	if err := dec.Decode(&raw); err != nil {
		sendError(conn, fmt.Sprintf("invalid request payload: %v", err))
		return
	}

	var ctrl ControlRequest
	if err := json.Unmarshal(raw, &ctrl); err == nil && (ctrl.SetModel != "" || ctrl.SetSystemPrompt != "" || ctrl.Reload) {
		if ctrl.Reload {
			loadModelsConfig()
		} else {
			state.mu.Lock()
			if ctrl.SetModel != "" {
				state.model = ctrl.SetModel
				log.Printf("Model set to: %s", state.model)
			}
			if ctrl.SetSystemPrompt != "" {
				state.systemPrompt = ctrl.SetSystemPrompt
				log.Printf("System prompt updated")
			}
			state.mu.Unlock()
		}
		sendOK(conn)
		return
	}

	var req Request
	if err := json.Unmarshal(raw, &req); err != nil {
		sendError(conn, fmt.Sprintf("invalid request payload: %v", err))
		return
	}

	var messages []Message

	state.mu.RLock()
	activeModel := req.Model
	if activeModel == "" {
		activeModel = state.model
	}
	systemPrompt := state.systemPrompt
	state.mu.RUnlock()

	if systemPrompt != "" {
		messages = append(messages, Message{Role: "system", Content: systemPrompt})
	}

	if len(req.Messages) > 0 {
		messages = append(messages, req.Messages...)
	} else if req.Prompt != "" {
		messages = append(messages, Message{Role: "user", Content: req.Prompt})
	} else {
		sendError(conn, "empty prompt and message list")
		return
	}

	streamLlama(conn, messages, activeModel, req.IncludeThinking)
}

// streamLlama calls llama-server's API and streams the output tokens back to the client socket.
func streamLlama(clientConn net.Conn, messages []Message, model string, includeThinking bool) {
	if model == "" {
		model = DefaultModel
	}

	chatReq := ChatCompletionRequest{
		Model:    model,
		Messages: messages,
		Stream:   true,
	}
	if !includeThinking {
		chatReq.ChatTemplateKwargs = map[string]any{"enable_thinking": false}
	}

	payloadBytes, err := json.Marshal(chatReq)
	if err != nil {
		sendError(clientConn, fmt.Sprintf("failed to serialize llama.cpp request: %v", err))
		return
	}

	// Connect to llama-server
	resp, err := http.Post(LlamaAPIURL, "application/json", bytes.NewBuffer(payloadBytes))
	if err != nil {
		sendError(clientConn, fmt.Sprintf("failed to contact llama.cpp service: %v. Is llama.cpp running?", err))
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		msg := strings.TrimSpace(string(body))
		if msg == "" {
			msg = fmt.Sprintf("llama.cpp returned status code %d", resp.StatusCode)
		}
		sendError(clientConn, msg)
		return
	}

	// Read from llama-server's stream and write to client
	scanner := bufio.NewScanner(resp.Body)
	encoder := json.NewEncoder(clientConn)

	for scanner.Scan() {
		payload, ok := parseSSELine(scanner.Text())
		if !ok {
			continue
		}

		if payload == "[DONE]" {
			if err := encoder.Encode(&ResponseChunk{Done: true}); err != nil {
				log.Printf("Failed to stream token to client: %v", err)
			}
			break
		}

		var chunk ChatCompletionChunk
		if err := json.Unmarshal([]byte(payload), &chunk); err != nil {
			log.Printf("Failed to unmarshal llama.cpp chunk: %v", err)
			continue
		}
		if len(chunk.Choices) == 0 {
			continue
		}

		token := chunk.Choices[0].Delta.Content
		var thinking string
		if includeThinking {
			thinking = chunk.Choices[0].Delta.ReasoningContent
		}

		if token == "" && thinking == "" {
			continue
		}

		clientChunk := ResponseChunk{
			Token:    token,
			Thinking: thinking,
		}

		if err := encoder.Encode(&clientChunk); err != nil {
			log.Printf("Failed to stream token to client: %v", err)
			return // Client disconnected
		}
	}

	if err := scanner.Err(); err != nil {
		sendError(clientConn, fmt.Sprintf("error reading stream from llama.cpp: %v", err))
	}
}

// sendError helper to transmit errors back to the client.
func sendError(conn net.Conn, msg string) {
	encoder := json.NewEncoder(conn)
	_ = encoder.Encode(&ResponseChunk{Error: msg, Done: true})
}

// sendOK confirms a control command was applied.
func sendOK(conn net.Conn) {
	encoder := json.NewEncoder(conn)
	_ = encoder.Encode(&ResponseChunk{Done: true})
}

// SendIPCCommand sends a command to brain.py via resonance.sock (for future tools)
func SendIPCCommand(command string) error {
	conn, err := net.Dial("unix", BrainSocketPath)
	if err != nil {
		return fmt.Errorf("failed to connect to resonance socket: %w", err)
	}
	defer conn.Close()

	payload := command
	if len(payload) == 0 || payload[len(payload)-1] != '\n' {
		payload += "\n"
	}

	_, err = conn.Write([]byte(payload))
	return err
}
