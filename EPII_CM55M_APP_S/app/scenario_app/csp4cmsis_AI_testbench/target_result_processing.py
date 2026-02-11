import os
import numpy as np
from sklearn.metrics import (accuracy_score, precision_score, recall_score,
                             f1_score, roc_auc_score, confusion_matrix,
                             precision_recall_curve, average_precision_score, roc_curve)
import pandas as pd
from pathlib import Path
import re

# --- Configuration ---
# Quantization parameters for de-quantizing MCU output (will be read from model_params.h)
OUTPUT_SCALE = None
OUTPUT_ZERO_POINT = None

# Ground truth definition for Atrial Fibrillation
AF_BEAT_THRESHOLD = 1 # 20 or more AF beats is considered a positive case

def read_model_params(file_path):
    """
    Reads OUTPUT_SCALE and OUTPUT_ZERO_POINT from model_params.h file
    """
    global OUTPUT_SCALE, OUTPUT_ZERO_POINT
    
    try:
        with open(file_path, 'r') as f:
            content = f.read()
            
            # Use regex to find the defines
            scale_match = re.search(r'#define\s+MODEL_OUTPUT_SCALE\s+([0-9.]+f?)', content)
            zero_point_match = re.search(r'#define\s+MODEL_OUTPUT_ZERO_POINT\s+(-?\d+)', content)
            
            if scale_match and zero_point_match:
                OUTPUT_SCALE = float(scale_match.group(1).rstrip('f'))
                OUTPUT_ZERO_POINT = int(zero_point_match.group(1))
                print(f"✅ Read model parameters: OUTPUT_SCALE={OUTPUT_SCALE}, OUTPUT_ZERO_POINT={OUTPUT_ZERO_POINT}")
            else:
                raise ValueError("Could not find required defines in model_params.h")
                
    except Exception as e:
        print(f"Error reading model parameters: {str(e)}")
        raise

def read_ground_truth(base_dir, index):
    """
    Reads the ground truth value (AF beat count) for a specific test vector.
    The file is expected to contain a single float32 value.
    """
    # Construct path based on the 2-level hex hash structure
    hex_str = f"{index:06x}"
    dir_path = os.path.join(base_dir, hex_str[:2], hex_str[2:4])
    file_path = os.path.join(dir_path, f"y_test_{index:06d}.bin")
    
    if not os.path.exists(file_path):
        return None
        
    try:
        # Read the single float32 value from the binary file
        with open(file_path, 'rb') as f:
            data = np.fromfile(f, dtype=np.float32)
            if len(data) > 0:
                return data[0] # Return the AF beat count
            return None
            
    except Exception as e:
        print(f"Error reading ground truth for sample {index}: {str(e)}")
        return None

def evaluate_classifier(predictions, ground_truths, pred_threshold=0.5):
    """Evaluates classifier performance with various metrics."""
    # Convert ground truth counts to binary labels based on the AF threshold
    gt_binary = (ground_truths >= AF_BEAT_THRESHOLD).astype(int)
    
    # Convert model's probability predictions to binary predictions
    binary_preds = (predictions >= pred_threshold).astype(int)
    
    # Calculate metrics
    metrics = {
        'accuracy': accuracy_score(gt_binary, binary_preds),
        'precision': precision_score(gt_binary, binary_preds, zero_division=0),
        'recall (sensitivity)': recall_score(gt_binary, binary_preds, zero_division=0),
        'f1': f1_score(gt_binary, binary_preds, zero_division=0),
        'roc_auc': roc_auc_score(gt_binary, predictions) if len(np.unique(gt_binary)) > 1 else float('nan'),
        'ap': average_precision_score(gt_binary, predictions),
        'confusion_matrix': confusion_matrix(gt_binary, binary_preds),
        'threshold': pred_threshold
    }
    
    return metrics

def find_optimal_threshold(predictions, ground_truths):
    """Finds the prediction threshold that maximizes the F1 score."""
    # Convert ground truth counts to binary labels
    gt_binary = (ground_truths >= AF_BEAT_THRESHOLD).astype(int)
    if len(np.unique(gt_binary)) < 2:
        return 0.5, 0.0  # Return default if only one class is present
    
    precisions, recalls, thresholds = precision_recall_curve(gt_binary, predictions)
    # Add a small epsilon to avoid division by zero
    f1_scores = 2 * (precisions * recalls) / (precisions + recalls + 1e-8)
    # The thresholds array is one element shorter than f1_scores
    optimal_idx = np.argmax(f1_scores[:-1])
    return thresholds[optimal_idx], f1_scores[optimal_idx]

def create_target_result_file(metrics, opt_metrics, output_path):
    """Creates the target_result.csv file with the specified format"""
    with open(output_path, 'w') as f:
        # Write TFLM Model parameters
        f.write("TFLM Model parameters:\n")
        f.write(f"OUTPUT_SCALE = {OUTPUT_SCALE}f\n")
        f.write(f"OUTPUT_ZERO_POINT = {OUTPUT_ZERO_POINT}\n\n")
        
        # Write performance at default threshold
        f.write("📊 Performance at default threshold=0.5:\n")
        for name, value in metrics.items():
            if name != 'confusion_matrix':
                f.write(f"  {name:20}: {value:.4f}\n")
        
        # Write confusion matrix
        tn, fp, fn, tp = metrics['confusion_matrix'].ravel()
        f.write("\n  Confusion matrix (TN, FP, FN, TP):\n")
        f.write(f"  [[{tn:^5}, {fp:^5}]\n")
        f.write(f"   [{fn:^5}, {tp:^5}]]\n\n")
        
        # Write performance at optimal threshold
        f.write("📊 Performance at optimal threshold={opt_threshold:.4f}:\n")
        for name, value in opt_metrics.items():
            if name != 'confusion_matrix':
                f.write(f"  {name:20}: {value:.4f}\n")
        
        # Write confusion matrix
        tn, fp, fn, tp = opt_metrics['confusion_matrix'].ravel()
        f.write("\n  Confusion matrix (TN, FP, FN, TP):\n")
        f.write(f"  [[{tn:^5}, {fp:^5}]\n")
        f.write(f"   [{fn:^5}, {tp:^5}]]\n")

def read_all_results_with_ground_truth(base_dir):
    """
    Walks the directory tree, finds bulk result files, and matches each of the 256
    predictions with its corresponding ground truth file.
    """
    results = []
    raw_predictions = []
    predictions = []
    ground_truths = []
    
    print(f"Searching for 'qdense_bulk_result_.bin' in '{base_dir}'...")
    
    for root, dirs, files in os.walk(base_dir):
        if "qdense_bulk_result_.bin" in files:
            bulk_file_path = os.path.join(root, "qdense_bulk_result_.bin")
            print(f"  Processing bulk file: {bulk_file_path}")

            try:
                # Get the directory parts to determine the starting index for this block
                path_parts = Path(root).parts
                dir2_hex, dir1_hex = path_parts[-1], path_parts[-2]
                
                # Calculate the starting index from the hex directory names
                start_index = (int(dir1_hex, 16) << 16) | (int(dir2_hex, 16) << 8)
                
                # Read all 256 raw predictions from the bulk file
                with open(bulk_file_path, 'rb') as f:
                    bulk_raw_preds = np.fromfile(f, dtype=np.int8)

                if len(bulk_raw_preds) != 256:
                    print(f"    Warning: Expected 256 results in {bulk_file_path}, but found {len(bulk_raw_preds)}. Skipping.")
                    continue

                # Process each of the 256 results in the file
                for i, raw_pred in enumerate(bulk_raw_preds):
                    current_index = start_index + i
                    raw_predictions.append(raw_pred)
                    # De-quantize the prediction to a float probability
                    normalized_pred = (raw_pred - OUTPUT_ZERO_POINT) * OUTPUT_SCALE
                    normalized_pred = np.clip(normalized_pred, 0.0, 1.0)
                    
                    # Read the corresponding ground truth (AF beat count)
                    gt_value = read_ground_truth(base_dir, current_index)
                    
                    if gt_value is not None:
                        results.append({
                            'index': current_index,
                            'prediction': normalized_pred,
                            'ground_truth': gt_value
                        })
                        predictions.append(normalized_pred)
                        ground_truths.append(gt_value)

            except Exception as e:
                print(f"    Error processing bulk file {bulk_file_path}: {e}")
    
    return results, np.array(predictions), np.array(ground_truths)

if __name__ == "__main__":
    # Read model parameters first
    MODEL_PARAMS_PATH = "model_params.h"
    try:
        read_model_params(MODEL_PARAMS_PATH)
    except Exception as e:
        print(f"Failed to read model parameters: {e}")
        exit(1)
    
    BASE_DIR = "/media/of6/TEST_DATA/blindfold_test_vectors"
    # Read all data
    results, predictions, ground_truths = read_all_results_with_ground_truth(BASE_DIR)
    
    if len(predictions) == 0:
        print("\nNo valid samples found with both predictions and ground truth. Exiting.")
        exit()
    
    print(f"\n✅ Successfully loaded and evaluated {len(predictions)} samples.")
    print("-" * 40)
    print(f"Ground truth is AF beat count (0-40). Threshold for positive class: >={AF_BEAT_THRESHOLD}")
    
    # Find and report optimal threshold
    opt_threshold, opt_f1 = find_optimal_threshold(predictions, ground_truths)
    print(f"\nOptimal threshold for maximizing F1-score: {opt_threshold:.4f} (Yields F1={opt_f1:.4f})")
    print("-" * 40)

    # Evaluate at the standard 0.5 threshold
    print("\n📊 Performance at default threshold=0.5:")
    metrics = evaluate_classifier(predictions, ground_truths)
    for name, value in metrics.items():
        if name != 'confusion_matrix':
            print(f"  {name:20}: {value:.4f}")
    print("\n  Confusion matrix (TN, FP, FN, TP):")
    tn, fp, fn, tp = metrics['confusion_matrix'].ravel()
    print(f"  [[{tn:^5}, {fp:^5}]")
    print(f"   [{fn:^5}, {tp:^5}]]")
    
    # Evaluate at the calculated optimal threshold
    print(f"\n📊 Performance at optimal threshold={opt_threshold:.4f}:")
    opt_metrics = evaluate_classifier(predictions, ground_truths, opt_threshold)
    for name, value in opt_metrics.items():
        if name != 'confusion_matrix':
            print(f"  {name:20}: {value:.4f}")
    print("\n  Confusion matrix (TN, FP, FN, TP):")
    tn, fp, fn, tp = opt_metrics['confusion_matrix'].ravel()
    print(f"  [[{tn:^5}, {fp:^5}]")
    print(f"   [{fn:^5}, {tp:^5}]]")
    print("-" * 40)

    # Save detailed results to CSV for further analysis
    df = pd.DataFrame(results)
    # Add columns for binary ground truth and the prediction based on the optimal threshold
    df['ground_truth_binary'] = (df['ground_truth'] >= AF_BEAT_THRESHOLD).astype(int)
    df['prediction_binary'] = (df['prediction'] >= opt_threshold).astype(int)
    df['correct'] = (df['ground_truth_binary'] == df['prediction_binary'])
    
    # Save to both CSV files
    output_csv_path = 'classification_results.csv'
    target_csv_path = 'target_result.csv'
    
    df.to_csv(output_csv_path, index=False)
    create_target_result_file(metrics, opt_metrics, target_csv_path)
    
    print(f"\n💾 Saved detailed results to '{output_csv_path}'")
    print(f"💾 Saved target results to '{target_csv_path}'")